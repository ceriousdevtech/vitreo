// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_x64_narrow_alu_5_lea_xchg_muldiv.cpp
/// @brief Long-mode IR emission: LEA, exchange, multiply and divide.

// These tests execute emitted ARM64, so Intel hosts must not reference the
// native-only IR backend through this translation unit.
#if defined(__aarch64__)
#include "ir_x64_memory_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_x64_memory;

namespace {
// LEA at all three long-mode widths. The interesting axis is the DESTINATION,
// not the address: the same effective address is kept whole at 64, truncated
// and zero-extended at 32, and truncated and MERGED at 16 -- three different
// register results from one computation. Flags must be untouched throughout,
// so every body is prefixed with a full-flag writer.
TEST_F(IrEmit64NarrowAlu, LoadEffectiveAddressAtEveryDestinationWidth) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    const auto with_flags = [&](std::vector<u8> tail) {
        std::vector<u8> body{0x48, 0x85, 0xDB};   // test rbx,rbx
        body.insert(body.end(), tail.begin(), tail.end());
        expect_alu_matches_interpreter(body, 2);
    };
    // 8D /r with the same modrm at each operand size: [rax], [rax+disp8],
    // [rax+rcx*4+disp8] and a SIB with no base.
    for (const std::vector<u8>& form : std::vector<std::vector<u8>>{
             {0x8D, 0x18},                                // [rax]
             {0x8D, 0x58, 0x7F},                          // [rax+127]
             {0x8D, 0x58, 0x80},                          // [rax-128]
             {0x8D, 0x9C, 0x88, 0x00, 0x10, 0x00, 0x00},  // [rax+rcx*4+0x1000]
             {0x8D, 0x1C, 0x8D, 0x00, 0x10, 0x00, 0x00},  // [rcx*4+0x1000]
             {0x8D, 0x1D, 0x00, 0x00, 0x00, 0x00}}) {     // [rip+0]
        with_flags(form);                                   // 32-bit dest
        std::vector<u8> word{0x66};
        word.insert(word.end(), form.begin(), form.end());
        with_flags(word);                                   // 16-bit dest
        std::vector<u8> quad{0x48};
        quad.insert(quad.end(), form.begin(), form.end());
        with_flags(quad);                                   // 64-bit dest
    }
    // An extended destination, and a destination that is also the base.
    with_flags({0x4C, 0x8D, 0x48, 0x10});     // lea r9, [rax+16]
    with_flags({0x48, 0x8D, 0x40, 0x10});     // lea rax, [rax+16]
    with_flags({0x66, 0x8D, 0x40, 0x10});     // lea ax, [rax+16]
    with_flags({0x8D, 0x40, 0x10});           // lea eax, [rax+16]
}

// CMPXCHG, register destination. Three registers are read and two written,
// and the aliased spellings are the ones that separate a correct lowering from
// a lucky one: `cmpxchg al, cl` names the accumulator as its own destination,
// where the equal path must leave the SOURCE, and `cmpxchg cl, cl` compares a
// register with itself. The flags are `accumulator - DEST`, measured on
// silicon -- see AtomicsMiscTest.CmpxchgFlagsAreAccumulatorMinusDestination.
TEST_F(IrEmit64NarrowAlu, CompareExchangeSelectsBothDestinations) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    const auto with_flags = [&](std::vector<u8> tail, unsigned insns = 2) {
        std::vector<u8> body{0x48, 0x85, 0xDB};   // test rbx,rbx
        body.insert(body.end(), tail.begin(), tail.end());
        expect_alu_matches_interpreter(body, insns);
    };
    with_flags({0x0F, 0xB0, 0xCB});          // cmpxchg bl, cl
    with_flags({0x0F, 0xB0, 0xC8});          // cmpxchg al, cl   (dst aliases acc)
    with_flags({0x0F, 0xB0, 0xC9});          // cmpxchg cl, cl   (dst aliases src)
    with_flags({0x0F, 0xB0, 0xC0});          // cmpxchg al, al   (all three)
    with_flags({0x0F, 0xB0, 0xEF});          // cmpxchg bh, ch
    with_flags({0x66, 0x0F, 0xB1, 0xCB});    // cmpxchg bx, cx
    with_flags({0x66, 0x0F, 0xB1, 0xC8});    // cmpxchg ax, cx
    with_flags({0x66, 0x0F, 0xB1, 0xC9});    // cmpxchg cx, cx
    with_flags({0x44, 0x0F, 0xB0, 0xCB});    // cmpxchg bl, r9b
    with_flags({0x41, 0x0F, 0xB0, 0xC9});    // cmpxchg r9b, cl
    // The flags must be readable by a following consumer, and the ZF the
    // record produces has to be the EQUALITY the instruction reports.
    with_flags({0x0F, 0xB0, 0xCB, 0x0F, 0x94, 0xC2}, 3);   // cmpxchg; sete dl
    with_flags({0x0F, 0xB0, 0xCB, 0x0F, 0x92, 0xC2}, 3);   // cmpxchg; setb dl
    with_flags({0x66, 0x0F, 0xB1, 0xCB, 0x0F, 0x9C, 0xC2}, 3);  // cmpxchg; setl dl
    // Driven at the exact operand pairs the silicon probe used, so an
    // inverted subtraction cannot hide behind the seed's values.
    struct Case { u64 acc; u64 dst; u64 src; };
    for (const Case c : {Case{0x01,0x02,0x55}, Case{0x02,0x01,0x55},
                         Case{0x80,0x01,0x55}, Case{0x01,0x80,0x55},
                         Case{0x00,0xFF,0x55}, Case{0xFF,0x00,0x55},
                         Case{0x42,0x42,0x55}}) {
        jit = std::make_unique<JitCompiler>(memory);
        const std::vector<u8> body{0x0F, 0xB0, 0xCB};
        auto* native = compile(body);
        ASSERT_NE(native, nullptr);
        auto oracle = seed();
        oracle.gpr.set64(0, 0xAAAAAAAAAAAAAA00ull | c.acc);
        oracle.gpr.set64(3, 0xBBBBBBBBBBBBBB00ull | c.dst);
        oracle.gpr.set64(1, 0xCCCCCCCCCCCCCC00ull | c.src);
        oracle.flags.rflags = 0x2D5;
        auto state = oracle;
        {
            Interpreter interpreter(oracle, memory);
            ASSERT_EQ(interpreter.step(), ExecResult::Continue);
        }
        ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
        for (u8 index = 0; index < 16; ++index)
            EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                << "GPR " << unsigned(index) << " acc=" << std::hex << c.acc
                << " dst=" << c.dst;
        state.materialize_lazy_flags();
        oracle.materialize_lazy_flags();
        EXPECT_EQ(state.flags.rflags, oracle.flags.rflags)
            << std::hex << " acc=" << c.acc << " dst=" << c.dst;
    }
}

// XCHG and XADD at the narrow widths, register forms. Both write TWO
// registers, and both can name the same register twice -- which is the case
// that tells a correct lowering from one that happens to agree: `xchg cl, cl`
// must leave CL alone, and `xadd cl, cl` must leave the SUM, because the
// reference writes the register operand first and the destination last.
TEST_F(IrEmit64NarrowAlu, ExchangeAndExchangeAddWriteBothOperands) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    const auto with_flags = [&](std::vector<u8> tail, unsigned insns = 2) {
        std::vector<u8> body{0x48, 0x85, 0xDB};   // test rbx,rbx
        body.insert(body.end(), tail.begin(), tail.end());
        expect_alu_matches_interpreter(body, insns);
    };
    // 86 /r, 87 /r: XCHG r8,r8 and r16,r16 -- no flag moves at all, so the
    // preceding TEST's flags must be exactly what remains.
    with_flags({0x86, 0xD9});               // xchg cl, bl
    with_flags({0x86, 0xE9});               // xchg ch, bl
    with_flags({0x86, 0xC9});               // xchg cl, cl  (aliased)
    with_flags({0x66, 0x87, 0xD9});         // xchg cx, bx
    with_flags({0x66, 0x87, 0xC9});         // xchg cx, cx  (aliased)
    with_flags({0x66, 0x91});               // xchg ax, cx
    with_flags({0x66, 0x93});               // xchg ax, bx
    with_flags({0x44, 0x86, 0xC9});         // xchg cl, r9b
    // 0F C0 /r, 0F C1 /r: XADD, which DOES define flags -- an ADD's.
    with_flags({0x0F, 0xC0, 0xD9});         // xadd cl, bl
    with_flags({0x0F, 0xC0, 0xE9});         // xadd cl, ch
    with_flags({0x0F, 0xC0, 0xC9});         // xadd cl, cl  (aliased)
    with_flags({0x66, 0x0F, 0xC1, 0xD9});   // xadd cx, bx
    with_flags({0x66, 0x0F, 0xC1, 0xC9});   // xadd cx, cx  (aliased)
    with_flags({0x44, 0x0F, 0xC0, 0xC9});   // xadd cl, r9b
    // The flags XADD leaves must be readable by a following consumer, not
    // just materialize the same -- a record that claimed the wrong operands
    // would agree on RFLAGS and disagree on a SETcc.
    with_flags({0x0F, 0xC0, 0xD9, 0x0F, 0x92, 0xC2}, 3);   // xadd; setb dl
    with_flags({0x66, 0x0F, 0xC1, 0xD9, 0x0F, 0x97, 0xC2}, 3);  // xadd; seta dl
}

// DIV and IDIV at 8 and 16 bits. The #DE path is the contract: a zero divisor
// or a quotient that does not fit must raise AND commit nothing, so the sweep
// drives dividends and divisors that land on both sides of every boundary --
// including the signed corners where the quotient is representable in one
// direction and not the other.
TEST_F(IrEmit64NarrowAlu, DivideRaisesAndCommitsNothingOnOverflow) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    unsigned raised = 0;
    unsigned completed = 0;
    struct Form { u8 ext; const char* name; bool word; };
    for (const Form form : {Form{6, "div", false}, Form{7, "idiv", false},
                            Form{6, "div", true},  Form{7, "idiv", true}}) {
        struct Case { u64 ax; u64 dx; u64 divisor; };
        for (const Case c : {Case{0, 0, 0},            // #DE: zero divisor
                             Case{0x0100, 0, 1},       // #DE: quotient > 0xFF
                             Case{0x00FF, 0, 1},       // exactly representable
                             Case{0x8000, 0, 0xFF},
                             Case{0x1234, 0x5678, 2},
                             Case{0xFFFF, 0xFFFF, 0xFFFF},
                             Case{0x8000, 0, 0xFFFF},  // signed corner
                             Case{0x0080, 0, 0xFF},
                             Case{0x7FFF, 0, 1},
                             Case{0x00FE, 0, 0x7F}}) {
            std::vector<u8> body;
            if (form.word) body.push_back(0x66);
            body.push_back(form.word ? 0xF7 : 0xF6);
            body.push_back(static_cast<u8>(0xC1 | (form.ext << 3)));  // cl / cx
            SCOPED_TRACE(testing::Message()
                         << form.name << (form.word ? "16" : "8")
                         << " ax=" << std::hex << c.ax << " dx=" << c.dx
                         << " div=" << c.divisor);
            jit = std::make_unique<JitCompiler>(memory);
            auto* native = compile(body);
            ASSERT_NE(native, nullptr);
            auto oracle = seed();
            oracle.gpr.set64(0, 0xAAAAAAAA00000000ull | c.ax);
            oracle.gpr.set64(2, 0xBBBBBBBB00000000ull | c.dx);
            oracle.gpr.set64(1, 0xCCCCCCCC00000000ull | c.divisor);
            oracle.flags.rflags = 0xAD7;
            auto state = oracle;
            Interpreter interpreter(oracle, memory);
            const auto ref = interpreter.step();
            const bool ref_raised = ref != ExecResult::Continue;
            if (ref_raised) ++raised; else ++completed;
            (void)jit->execute(*native, state);
            EXPECT_EQ(state.pending_raise.active, ref_raised) << "#DE agreement";
            for (u8 index = 0; index < 16; ++index)
                EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                    << "GPR " << unsigned(index);
            EXPECT_EQ(state.rip, oracle.rip) << "faulting RIP";
        }
    }
    // A memory divisor, and the faulting-load ordering.
    for (const u8 ext : {u8{6}, u8{7}}) {
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile({0xF6, static_cast<u8>(0x00 | (ext << 3))});
        ASSERT_NE(native, nullptr);
        auto oracle = seed();
        oracle.gpr.set64(0, kData);
        ASSERT_TRUE(memory.write_u64(kData, 3).is_ok());
        oracle.gpr.set64(0, kData);
        auto state = oracle;
        Interpreter interpreter(oracle, memory);
        const auto ref = interpreter.step();
        (void)jit->execute(*native, state);
        EXPECT_EQ(state.pending_raise.active, ref != ExecResult::Continue);
        for (u8 index = 0; index < 16; ++index)
            EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                << "GPR " << unsigned(index);
    }
    EXPECT_GT(raised, 8u) << "the #DE path must actually be taken";
    EXPECT_GT(completed, 8u) << "and the ordinary path too";
}

// DIV and IDIV at 32 and 64 bits (Intel SDM Vol. 2A): EDX:EAX or RDX:RAX by
// r/m. The 32-bit forms write EAX/EDX, clearing bits 63:32; #DE commits
// nothing. The divisor may itself be RAX or RDX, which the helper must read
// before either is written.
TEST_F(IrEmit64NarrowAlu, WideDivideMatchesTheInterpreterAndTheSdm) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    constexpr u64 kMin64 = 0x8000000000000000ull;
    struct Case { u64 rax; u64 rdx; u64 divisor; };
    const std::vector<Case> cases{
        {0, 0, 0},                                    // #DE: zero divisor
        {0x123456789ull, 0, 7},
        {0, 1, 1},                                    // 32: fits; 64: #DE
        {0xFFFFFFFFull, 0, 1},
        {0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull},
        {0x80000000ull, 0xFFFFFFFFull, 0xFFFFFFFFull},  // 32 signed corner
        {0x80000000ull, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFull},
        {0, 0x80000000ull, 0xFFFFFFFFull},            // INT64_MIN / -1 (32)
        {0, kMin64, 0xFFFFFFFFFFFFFFFFull},           // -2^127 / -1 (64)
        {kMin64, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull},
        {kMin64, 0, 2},
        {0x7FFFFFFFFFFFFFFFull, 0, 0xFFFFFFFFFFFFFFFFull},
        {0xDEADBEEFCAFEBABEull, 0x12345ull, 0xFFFFFFFFFFFFull},
        {5, 0xFFFFFFFFFFFFFFFFull, 3},                // negative dividend
    };
    unsigned raised = 0, completed = 0;
    struct Form { u8 ext; bool wide; const char* name; };
    for (const Form form : {Form{6, false, "div32"}, Form{7, false, "idiv32"},
                            Form{6, true, "div64"}, Form{7, true, "idiv64"}}) {
        // rcx, rax and rdx as the divisor, then a memory divisor.
        for (const u8 rm : {u8{1}, u8{0}, u8{2}, u8{0xFF}}) {
            std::vector<u8> body;
            if (form.wide) body.push_back(0x48);
            body.push_back(0xF7);
            body.push_back(rm == 0xFF ? static_cast<u8>(0x03 | (form.ext << 3))
                                      : static_cast<u8>(0xC0 | (form.ext << 3) | rm));
            jit = std::make_unique<JitCompiler>(memory);
            auto* native = compile(body);
            ASSERT_NE(native, nullptr);
            for (const Case& c : cases) {
                SCOPED_TRACE(testing::Message()
                             << form.name << " rm=" << unsigned(rm) << std::hex
                             << " rax=" << c.rax << " rdx=" << c.rdx
                             << " div=" << c.divisor);
                auto oracle = seed();
                for (u8 index = 0; index < 16; ++index)
                    oracle.gpr.set64(index, 0x0F0F0F0F0F0F0F0Full * (index + 1));
                oracle.gpr.rax = c.rax;
                oracle.gpr.rdx = c.rdx;
                if (rm == 1) oracle.gpr.rcx = c.divisor;
                oracle.gpr.rbx = kData;
                ASSERT_TRUE(memory.write_u64(kData, c.divisor).is_ok());
                auto state = oracle;
                Interpreter interpreter(oracle, memory);
                const bool ref_raised = interpreter.step() != ExecResult::Continue;
                if (ref_raised) ++raised; else ++completed;
                (void)jit->execute(*native, state);
                EXPECT_EQ(state.pending_raise.active, ref_raised) << "#DE agreement";
                for (u8 index = 0; index < 16; ++index)
                    EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                        << "GPR " << unsigned(index);
                EXPECT_EQ(state.rip, oracle.rip);
            }
        }
    }
    EXPECT_GT(raised, 16u);
    EXPECT_GT(completed, 16u);

    // Literal SDM results, so the interpreter is not the only reference.
    struct Literal {
        std::vector<u8> bytes; u64 rax, rdx, rcx;
        bool de; u64 want_rax, want_rdx;
    };
    for (const Literal& l : std::vector<Literal>{
             // div ecx: 0x1_00000005 / 2 = 0x80000002 r 1, upper halves cleared.
             {{0xF7, 0xF1}, 0xAAAAAAAA00000005ull, 0xBBBBBBBB00000001ull, 2,
              false, 0x80000002ull, 1},
             // idiv ecx: -7 / 2 = -3 r -1 (truncation toward zero).
             {{0xF7, 0xF9}, 0xFFFFFFF9ull, 0xFFFFFFFFull, 2,
              false, 0xFFFFFFFDull, 0xFFFFFFFFull},
             // idiv ecx: 0x80000000 / -1 overflows 32 bits.
             {{0xF7, 0xF9}, 0x80000000ull, 0xFFFFFFFFull, 0xFFFFFFFFull,
              true, 0x80000000ull, 0xFFFFFFFFull},
             // div rcx: 2^64 / 2 = 2^63 r 0.
             {{0x48, 0xF7, 0xF1}, 0, 1, 2, false, kMin64, 0},
             // idiv rcx: -2^63 / -1 overflows 64 bits.
             {{0x48, 0xF7, 0xF9}, kMin64, 0xFFFFFFFFFFFFFFFFull,
              0xFFFFFFFFFFFFFFFFull, true, kMin64, 0xFFFFFFFFFFFFFFFFull},
             // idiv rcx: -2^127 / -1 overflows 64 bits.
             {{0x48, 0xF7, 0xF9}, 0, kMin64, 0xFFFFFFFFFFFFFFFFull,
              true, 0, kMin64},
         }) {
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(l.bytes);
        ASSERT_NE(native, nullptr);
        auto state = seed();
        state.gpr.rax = l.rax;
        state.gpr.rdx = l.rdx;
        state.gpr.rcx = l.rcx;
        (void)jit->execute(*native, state);
        SCOPED_TRACE(testing::Message() << std::hex << "rax=" << l.rax);
        EXPECT_EQ(state.pending_raise.active, l.de);
        EXPECT_EQ(state.gpr.rax, l.want_rax);
        EXPECT_EQ(state.gpr.rdx, l.want_rdx);
        EXPECT_EQ(state.rip, l.de ? kCode : kCode + l.bytes.size());
    }
}

// MUL/IMUL's one-operand forms write the implicit accumulator: AX for the byte
// form, DX:AX for the word form. Both the second destination and the signed vs
// unsigned overflow rule are things a lowering can get right for one width and
// wrong for the other, so every shape runs against every seed, and the bodies
// are prefixed with a full-flag writer whose ZF/SF/PF must survive.
TEST_F(IrEmit64NarrowAlu, AccumulatorMultiplyWritesTheImplicitPair) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    const auto with_flags = [&](std::vector<u8> tail) {
        std::vector<u8> body{0x48, 0x85, 0xDB};   // test rbx,rbx
        body.insert(body.end(), tail.begin(), tail.end());
        expect_alu_matches_interpreter(body, 2);
    };
    for (const u8 ext : {u8{4}, u8{5}}) {         // MUL /4, IMUL /5
        with_flags({0xF6, static_cast<u8>(0xC1 | (ext << 3))});        // cl
        with_flags({0xF6, static_cast<u8>(0xC5 | (ext << 3))});        // ch
        with_flags({0xF6, static_cast<u8>(0xC0 | (ext << 3))});        // al (aliases AX)
        with_flags({0xF6, static_cast<u8>(0x00 | (ext << 3))});        // byte [rax]
        with_flags({0x66, 0xF7, static_cast<u8>(0xC1 | (ext << 3))});  // cx
        with_flags({0x66, 0xF7, static_cast<u8>(0xC2 | (ext << 3))});  // dx (aliases the high half)
        with_flags({0x66, 0xF7, static_cast<u8>(0xC0 | (ext << 3))});  // ax
        with_flags({0x66, 0xF7, static_cast<u8>(0x00 | (ext << 3))});  // word [rax]
        with_flags({0x41, 0xF6, static_cast<u8>(0xC1 | (ext << 3))});  // r9b
    }
    // Operands chosen around the fit boundary at both widths, driven into the
    // accumulator directly rather than left to the seed.
    struct Case { u64 a; u64 b; };
    for (const Case c : {Case{0, 0}, Case{1, 1}, Case{0x7F, 0x7F}, Case{0x80, 0x80},
                         Case{0xFF, 0xFF}, Case{0x7FFF, 0x7FFF},
                         Case{0x8000, 0x8000}, Case{0xFFFF, 0xFFFF},
                         Case{0x100, 0x100}, Case{0xFFFF, 1}}) {
        for (const std::vector<u8>& tail : std::vector<std::vector<u8>>{
                 {0xF6, 0xE1}, {0xF6, 0xE9}, {0x66, 0xF7, 0xE1}, {0x66, 0xF7, 0xE9}}) {
            jit = std::make_unique<JitCompiler>(memory);
            auto* native = compile(tail);
            ASSERT_NE(native, nullptr);
            auto oracle = seed();
            oracle.gpr.set64(0, c.a);
            oracle.gpr.set64(1, c.b);
            oracle.gpr.set64(2, 0xDEADBEEFCAFEBABEull);
            oracle.flags.rflags = 0xAD7;
            auto state = oracle;
            {
                Interpreter interpreter(oracle, memory);
                ASSERT_EQ(interpreter.step(), ExecResult::Continue);
            }
            ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
            for (u8 index = 0; index < 16; ++index)
                EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                    << "GPR " << unsigned(index) << " for " << std::hex
                    << c.a << " * " << c.b;
            state.materialize_lazy_flags();
            oracle.materialize_lazy_flags();
            EXPECT_EQ(state.flags.rflags, oracle.flags.rflags)
                << std::hex << c.a << " * " << c.b;
        }
    }
}

// IMUL's two- and three-operand forms. They define CF and OF only -- both
// meaning "the product did not fit the destination" -- and leave ZF/SF/PF/AF
// alone, so each body is prefixed with a full-flag writer whose ZF/SF/PF must
// still be standing afterwards. The immediates walk the sign boundary, since
// an immediate zero-extended instead of sign-extended gives the right low half
// and the wrong overflow answer.
TEST_F(IrEmit64NarrowAlu, IntegerMultiplyDefinesOnlyCarryAndOverflow) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    const auto with_flags = [&](std::vector<u8> tail) {
        std::vector<u8> body{0x48, 0x85, 0xDB};   // test rbx,rbx
        body.insert(body.end(), tail.begin(), tail.end());
        expect_alu_matches_interpreter(body, 2);
    };
    // 0F AF /r: two-operand, at all three widths, register and memory source.
    with_flags({0x66, 0x0F, 0xAF, 0xD9});          // imul bx, cx
    with_flags({0x0F, 0xAF, 0xD9});                // imul ebx, ecx
    with_flags({0x48, 0x0F, 0xAF, 0xD9});          // imul rbx, rcx
    with_flags({0x66, 0x0F, 0xAF, 0x18});          // imul bx, [rax]
    with_flags({0x0F, 0xAF, 0x18});                // imul ebx, [rax]
    with_flags({0x48, 0x0F, 0xAF, 0x18});          // imul rbx, [rax]
    with_flags({0x4D, 0x0F, 0xAF, 0xD9});          // imul r11, r9
    // 6B /r ib: three-operand with a sign-extended imm8.
    for (const u8 imm : {u8{0}, u8{1}, u8{2}, u8{0x7F}, u8{0x80}, u8{0xFF}}) {
        with_flags({0x66, 0x6B, 0xD9, imm});       // imul bx, cx, imm8
        with_flags({0x6B, 0xD9, imm});             // imul ebx, ecx, imm8
        with_flags({0x48, 0x6B, 0xD9, imm});       // imul rbx, rcx, imm8
        with_flags({0x66, 0x6B, 0x18, imm});       // imul bx, [rax], imm8
        with_flags({0x6B, 0x18, imm});             // imul ebx, [rax], imm8
        with_flags({0x48, 0x6B, 0x18, imm});       // imul rbx, [rax], imm8
    }
    // 69 /r iw/id: three-operand with a full-width immediate.
    with_flags({0x66, 0x69, 0xD9, 0x00, 0x80});                    // imul bx,cx,-32768
    with_flags({0x66, 0x69, 0xD9, 0xFF, 0x7F});                    // imul bx,cx,32767
    with_flags({0x69, 0xD9, 0x00, 0x00, 0x00, 0x80});              // imul ebx,ecx,INT32_MIN
    with_flags({0x69, 0xD9, 0xFF, 0xFF, 0xFF, 0x7F});              // imul ebx,ecx,INT32_MAX
    with_flags({0x48, 0x69, 0xD9, 0xFF, 0xFF, 0xFF, 0x7F});        // imul rbx,rcx,INT32_MAX
    with_flags({0x48, 0x69, 0xD9, 0x00, 0x00, 0x00, 0x80});        // imul rbx,rcx,INT32_MIN
    // Products chosen to land on both sides of the fit boundary at 64 bits,
    // where the answer needs the high half of a 128-bit product.
    struct Pair { u64 a; u64 b; };
    for (const Pair pair : {Pair{2, 3}, Pair{~0ull, ~0ull}, Pair{1ull << 62, 4},
                            Pair{0x7FFFFFFFFFFFFFFFull, 2},
                            Pair{0x8000000000000000ull, ~0ull},
                            Pair{0xFFFFFFFFull, 0xFFFFFFFFull}}) {
        jit = std::make_unique<JitCompiler>(memory);
        const std::vector<u8> body{0x48, 0x85, 0xDB, 0x48, 0x0F, 0xAF, 0xD9};
        auto* native = compile(body);
        ASSERT_NE(native, nullptr);
        auto oracle = seed();
        oracle.gpr.set64(3, pair.a);
        oracle.gpr.set64(1, pair.b);
        oracle.flags.rflags = 0xAD7;
        auto state = oracle;
        {
            Interpreter interpreter(oracle, memory);
            for (unsigned step = 0; step < 2; ++step)
                ASSERT_EQ(interpreter.step(), ExecResult::Continue);
        }
        ASSERT_EQ(jit->execute(*native, state), JitExecResult::Continue);
        EXPECT_EQ(state.gpr.get64(3), oracle.gpr.get64(3))
            << std::hex << pair.a << " * " << pair.b;
        state.materialize_lazy_flags();
        oracle.materialize_lazy_flags();
        EXPECT_EQ(state.flags.rflags, oracle.flags.rflags)
            << std::hex << pair.a << " * " << pair.b;
    }
}

// The quotient and remainder a wide DIV/IDIV writes must be what LATER
// instructions in the same block read, not only what the block leaves in the
// register file. Sequences from a real x64 image (d3dcompiler_47 range
// checks): the quotient feeds a CMP whose Jcc picks the exit, a MOVSXD, a
// store and an address; the remainder feeds an ADD.
TEST_F(IrEmit64NarrowAlu, WideDivideResultsFeedLaterInstructionsInTheBlock) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    struct Body { const char* what; std::vector<u8> bytes; unsigned steps; };
    const std::vector<Body> bodies{
        {"mov eax,INT32_MAX; cqo; idiv r9; cmp rcx,rax; jg",
         {0xB8, 0xFF, 0xFF, 0xFF, 0x7F, 0x48, 0x99, 0x49, 0xF7, 0xF9,
          0x48, 0x3B, 0xC8, 0x0F, 0x8F, 0x00, 0x01, 0x00, 0x00}, 5},
        {"mov rax,INT32_MIN; cqo; idiv r9; cmp rcx,rax; jg",
         {0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x80, 0x48, 0x99, 0x49, 0xF7,
          0xF9, 0x48, 0x3B, 0xC8, 0x0F, 0x8F, 0x00, 0x01, 0x00, 0x00}, 5},
        {"cdq; idiv ecx; movsxd rcx,eax; mov [rbx],rcx",
         {0x99, 0xF7, 0xF9, 0x48, 0x63, 0xC8, 0x48, 0x89, 0x0B}, 4},
        {"xor edx,edx; div r9; lea rsi,[rax+rdx*2]; add rdx,rax",
         {0x31, 0xD2, 0x49, 0xF7, 0xF1, 0x48, 0x8D, 0x34, 0x50, 0x48, 0x01,
          0xC2}, 4},
        {"cdq; idiv dword [rbx+8]; imul eax,edx; sub eax,ecx",
         {0x99, 0xF7, 0x7B, 0x08, 0x0F, 0xAF, 0xC2, 0x29, 0xC8}, 4},
    };
    unsigned completed = 0;
    for (const Body& body : bodies) {
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(body.bytes);
        ASSERT_NE(native, nullptr) << body.what;
        for (const u64 divisor : {u64{3}, u64{7}, ~u64{0}, u64{0x10000},
                                  u64{0xFFFFFFF9}, u64{0x123456789}}) {
            SCOPED_TRACE(testing::Message() << body.what << " divisor="
                                            << std::hex << divisor);
            auto oracle = seed();
            for (u8 index = 0; index < 16; ++index)
                oracle.gpr.set64(index, 0x0F0F0F0F0F0F0F0Full * (index + 1));
            oracle.gpr.rax = 0x00000000DEADBEEFull;
            oracle.gpr.rcx = divisor;
            oracle.gpr.r9 = divisor;
            oracle.gpr.rbx = kData;
            ASSERT_TRUE(memory.write_u64(kData, 0).is_ok());
            ASSERT_TRUE(memory.write_u64(kData + 8, divisor).is_ok());
            auto state = oracle;
            bool ref_raised = false;
            {
                Interpreter interpreter(oracle, memory);
                for (unsigned step = 0; step < body.steps && !ref_raised; ++step) {
                    const ExecResult r = interpreter.step();
                    ref_raised = r != ExecResult::Continue && r != ExecResult::Branch;
                }
            }
            const auto oracle_memory = memory.read_u64(kData);
            ASSERT_TRUE(memory.write_u64(kData, 0).is_ok());
            (void)jit->execute(*native, state);
            EXPECT_EQ(state.pending_raise.active, ref_raised) << "#DE agreement";
            if (!ref_raised) ++completed;
            EXPECT_EQ(memory.read_u64(kData).value_or(1), oracle_memory.value_or(2));
            for (u8 index = 0; index < 16; ++index)
                EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                    << "GPR " << unsigned(index);
            EXPECT_EQ(state.rip, oracle.rip);
            state.materialize_lazy_flags();
            oracle.materialize_lazy_flags();
            // DIV/IDIV leave every flag undefined; each body ends in a writer.
            EXPECT_EQ(state.flags.rflags, oracle.flags.rflags);
        }
    }
    EXPECT_GT(completed, 20u);
}

// #DE is a fault (Intel SDM Vol. 3A 6.15, interrupt 0): the DIV commits
// nothing, so the exception context carries the EFLAGS of the instruction
// before it. Here that is `xor edx, edx` (ZF=PF=1), whose record the later CMP
// kills -- a record Tier 1 may elide only if the #DE exit rebuilds it.
// Sequences from a real x64 image (d3dcompiler_47 size checks).
TEST_F(IrEmit64NarrowAlu, WideDivideFaultCarriesTheFlagsBeforeTheDivide) {
    struct Body { const char* what; std::vector<u8> bytes; unsigned divide_at; };
    const std::vector<Body> bodies{
        {"xor edx,edx; mov eax,-1; div edi; cmp r14d,eax",
         {0x33, 0xD2, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0xF7, 0x44, 0x3B,
          0xF0}, 7},
        {"xor edx,edx; mov eax,edi; div ecx; test edx,edx",
         {0x33, 0xD2, 0x8B, 0xC7, 0xF7, 0xF1, 0x85, 0xD2}, 4},
        {"xor edx,edx; mov rax,r13; div rcx; test rdx,rdx",
         {0x33, 0xD2, 0x49, 0x8B, 0xC5, 0x48, 0xF7, 0xF1, 0x48, 0x85, 0xD2}, 5},
    };
    for (const Body& body : bodies) {
        SCOPED_TRACE(body.what);
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(body.bytes);
        ASSERT_NE(native, nullptr);
        auto oracle = seed();
        for (u8 index = 0; index < 16; ++index)
            oracle.gpr.set64(index, 0x0F0F0F0F0F0F0F0Full * (index + 1));
        oracle.gpr.rcx = 0;
        oracle.gpr.rdi = 0;
        oracle.flags.rflags = 0x8D7;  // OF SF AF PF CF set, ZF clear
        auto state = oracle;
        {
            Interpreter interpreter(oracle, memory);
            ASSERT_EQ(interpreter.step(), ExecResult::Continue);
            ASSERT_EQ(interpreter.step(), ExecResult::Continue);
            ASSERT_NE(interpreter.step(), ExecResult::Continue) << "#DE";
        }
        (void)jit->execute(*native, state);
        ASSERT_TRUE(state.pending_raise.active);
        EXPECT_EQ(state.pending_raise.code, 0xC0000094u);  // STATUS_INTEGER_DIVIDE_BY_ZERO
        EXPECT_EQ(state.rip, kCode + body.divide_at);
        EXPECT_EQ(state.rip, oracle.rip);
        for (u8 index = 0; index < 16; ++index)
            EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index))
                << "GPR " << unsigned(index);
        state.materialize_lazy_flags();
        oracle.materialize_lazy_flags();
        EXPECT_EQ(state.flags.rflags & 0x8D5u, oracle.flags.rflags & 0x8D5u);
        EXPECT_EQ(oracle.flags.rflags & 0x8C5u, 0x44u) << "XOR: ZF=PF=1";
    }
}
} // namespace
#endif
