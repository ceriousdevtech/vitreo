// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_x64_carry_chain.cpp
/// @brief Long-mode IR carry chains: ADD/SUB/CMP feeding ADC/SBB with CF
///        kept in host C, against the interpreter.
///
/// Intel SDM Vol. 2A ADC/SBB: DEST + SRC + CF and DEST - (SRC + CF), every
/// status flag from that three-way result. A chain whose intermediate records
/// the IR keeps only in PSTATE must agree with the interpreter on all sixteen
/// GPRs and the materialized RFLAGS, whatever follows it: nothing, a full
/// writer, a CF reader (SETB), or a CF-preserving INC that merges the carry.

#if defined(__aarch64__)
#include "ir_x64_memory_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_x64_memory;

namespace {

struct Piece { std::vector<u8> bytes; const char* name; };

std::vector<u8> cat(std::initializer_list<const std::vector<u8>*> parts) {
    std::vector<u8> out;
    for (const auto* p : parts) out.insert(out.end(), p->begin(), p->end());
    return out;
}

// RAX holds the fixture's data address, so the chains use the other
// registers; their seeds cross the sign and carry boundaries.
const std::vector<Piece>& producers() {
    static const std::vector<Piece> p{
        {{0x01, 0xD1}, "add ecx,edx"},
        {{0x48, 0x01, 0xD1}, "add rcx,rdx"},
        {{0x83, 0xC1, 0x01}, "add ecx,1"},
        {{0x48, 0x83, 0xC1, 0xFF}, "add rcx,-1"},
        {{0x29, 0xCA}, "sub edx,ecx"},
        {{0x48, 0x29, 0xCA}, "sub rdx,rcx"},
        {{0x39, 0xD1}, "cmp ecx,edx"},
        {{0x48, 0x39, 0xD1}, "cmp rcx,rdx"},
        {{0xFF, 0xC1}, "inc ecx"},
        {{0xF7, 0xD9}, "neg ecx"},
    };
    return p;
}

const std::vector<Piece>& carry_ops() {
    static const std::vector<Piece> c{
        {{0x11, 0xDE}, "adc esi,ebx"},
        {{0x19, 0xDE}, "sbb esi,ebx"},
        {{0x83, 0xD2, 0x00}, "adc edx,0"},
        {{0x83, 0xDF, 0xFF}, "sbb edi,-1"},
        {{0x11, 0xFF}, "adc edi,edi"},
        {{0x19, 0xF6}, "sbb esi,esi"},
        {{0x10, 0xD1}, "adc cl,dl"},
        {{0x66, 0x19, 0xD1}, "sbb cx,dx"},
    };
    return c;
}

class IrX64CarryChain : public IrEmit64NarrowAlu {};

TEST_F(IrX64CarryChain, ChainsMatchTheInterpreterWhateverFollows) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    const std::vector<Piece> second{
        {{0x19, 0xCF}, "sbb edi,ecx"},
        {{0x11, 0xCB}, "adc ebx,ecx"},
        {{0x83, 0xD6, 0x00}, "adc esi,0"},
        {{0x10, 0xF2}, "adc dl,dh"},
    };
    const std::vector<Piece> tails{
        {{}, "(end)"},
        {{0x83, 0xC5, 0x01}, "add ebp,1"},
        {{0x0F, 0x92, 0xC0}, "setb al"},
        {{0xFF, 0xC5}, "inc ebp"},
    };
    for (const Piece& p : producers()) {
        for (const Piece& c : carry_ops()) {
            for (const Piece& s : second) {
                for (const Piece& t : tails) {
                    SCOPED_TRACE(testing::Message() << p.name << " ; " << c.name
                                 << " ; " << s.name << " ; " << t.name);
                    const auto body = cat({&p.bytes, &c.bytes, &s.bytes, &t.bytes});
                    expect_alu_matches_interpreter(body, t.bytes.empty() ? 3 : 4);
                }
            }
        }
    }
}

// The bench shape: carry rippling through a run of ADD ; ADC ; SBB, with a
// CF-merging DEC at the end reading the last SBB's record.
TEST_F(IrX64CarryChain, RepeatedAddAdcSbbRunMatchesTheInterpreter) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    const std::vector<u8> step{0x83, 0xC1, 0x01,    // add ecx,1
                               0x83, 0xD2, 0x00,    // adc edx,0
                               0x83, 0xDE, 0x00};   // sbb esi,0
    std::vector<u8> body;
    for (int i = 0; i < 4; ++i) body.insert(body.end(), step.begin(), step.end());
    body.insert(body.end(), {0x48, 0xFF, 0xCB});    // dec rbx
    expect_alu_matches_interpreter(body, 13);
}

// What the chain lowers to. ADDS leaves C == CF, which ADC consumes as is;
// ADCS leaves C == CF, and SBC needs C == NOT CF -- one CFINV. No condition
// is materialized with a CSET, and the two intermediate definitions keep no
// lazy record (the SBB's is killed by the final ADD).
TEST_F(IrX64CarryChain, AddAdcSbbKeepsTheCarryInHostC) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    const std::vector<u8> body{0x83, 0xC1, 0x01,    // add ecx,1
                               0x83, 0xD2, 0x00,    // adc edx,0
                               0x83, 0xDE, 0x00,    // sbb esi,0
                               0x83, 0xC5, 0x01};   // add ebp,1
    auto* native = compile(body);
    ASSERT_NE(native, nullptr);
    u32 pstate = 0;
    for (const ir::Inst& in : jit->ir_block().insts) {
        if (in.op == ir::Op::DefFlags && in.flags.repr == ir::FlagRepr::InPstate)
            ++pstate;
    }
    EXPECT_EQ(pstate, 2u) << ir::print(jit->ir_block());
    const auto* words = static_cast<const u32*>(native->code);
    const usize n = native->code_size / sizeof(u32);
    u32 adcs = 0, sbcs = 0, cfinv = 0, cset = 0;
    for (usize i = 0; i < n; ++i) {
        const u32 w = words[i];
        if ((w & 0xFFE0FC00u) == 0x3A000000u) ++adcs;
        if ((w & 0xFFE0FC00u) == 0x7A000000u) ++sbcs;
        if (w == arm64::cfinv()) ++cfinv;
        // CSET Wd, cond == CSINC Wd, WZR, WZR, invert(cond).
        if ((w & 0xFFFF0FE0u) == 0x1A9F07E0u) ++cset;
    }
    EXPECT_EQ(adcs, 1u);
    EXPECT_EQ(sbcs, 1u);
    EXPECT_EQ(cfinv, 1u);
    EXPECT_EQ(cset, 0u);
    expect_alu_matches_interpreter(body, 4);
}

// The LZSS match-length clamp: `cmp ; cmovb ; add ; cmp`. The CMOVB's CF is
// read off the compare's live claim by a CSEL, and the ADD's flags then kill
// the record before anything could observe it, so the compare publishes no
// record (Intel SDM Vol. 2A CMOVcc: reads CF, writes no flags). Every result
// and the final flags are the interpreter's.
TEST_F(IrX64CarryChain, CarryCmovOffALiveCompareStoresNoRecord) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    const std::vector<std::vector<u8>> cmovs{
        {0x0F, 0x42, 0xCA},          // cmovb ecx,edx
        {0x0F, 0x43, 0xCA},          // cmovae ecx,edx
        {0x48, 0x0F, 0x42, 0xCA},    // cmovb rcx,rdx
    };
    const std::vector<u8> cmp{0x39, 0xD1};                     // cmp ecx,edx
    const std::vector<u8> tail{0x83, 0xC6, 0x01,               // add esi,1
                               0x39, 0xFE};                    // cmp esi,edi
    for (const auto& c : cmovs) {
        SCOPED_TRACE(testing::Message() << std::hex << unsigned(c[c.size() - 2]));
        const auto body = cat({&cmp, &c, &tail});
        jit = std::make_unique<JitCompiler>(memory);
        auto* native = compile(body);
        ASSERT_NE(native, nullptr);
        u32 pstate = 0;
        for (const ir::Inst& in : jit->ir_block().insts) {
            if (in.op == ir::Op::DefFlags && in.flags.repr == ir::FlagRepr::InPstate)
                ++pstate;
        }
        EXPECT_EQ(pstate, 1u) << ir::print(jit->ir_block());
        expect_alu_matches_interpreter(body, 4);
    }
    // A condition the claim does not serve as a carry keeps its record.
    const std::vector<u8> cmovz{0x0F, 0x44, 0xCA};
    expect_alu_matches_interpreter(cat({&cmp, &cmovz, &tail}), 4);
}

// A fault between a chain's links: the carry must not be kept only in PSTATE
// across it (the handler cannot see NZCV), and the exception state is the
// interpreter's -- the ADC's record, GPRs before the store.
TEST_F(IrX64CarryChain, FaultInsideAChainPublishesTheCarryProducersRecord) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::Read));
    const std::vector<u8> body{0x01, 0xD1,          // add ecx,edx
                               0x11, 0xDE,          // adc esi,ebx
                               0x4C, 0x89, 0x08,    // mov [rax],r9 (faults)
                               0x19, 0xCF};         // sbb edi,ecx
    auto* native = compile(body);
    ASSERT_NE(native, nullptr);
    for (const u64 v : {0ull, 0xFFFFFFFFFFFFFFFFull, 0x7FFFFFFF80000000ull}) {
        SCOPED_TRACE(testing::Message() << std::hex << v);
        auto oracle = seed();
        oracle.gpr.rax = kData;
        oracle.gpr.rcx = v;
        oracle.gpr.rdx = ~v ^ 0x1234u;
        oracle.gpr.rbx = v >> 1;
        oracle.gpr.rsi = 0x80000000u;
        auto state = oracle;
        {
            Interpreter interpreter(oracle, memory);
            ASSERT_EQ(interpreter.step(), ExecResult::Continue);
            ASSERT_EQ(interpreter.step(), ExecResult::Continue);
            ASSERT_NE(interpreter.step(), ExecResult::Continue);
        }
        (void)jit->execute(*native, state);
        ASSERT_TRUE(state.pending_raise.active);
        EXPECT_EQ(state.pending_raise.code, 0xC0000005u);
        EXPECT_EQ(state.rip, kCode + 4);
        for (u8 index = 0; index < 16; ++index)
            EXPECT_EQ(state.gpr.get64(index), oracle.gpr.get64(index)) << unsigned(index);
        state.materialize_lazy_flags();
        oracle.materialize_lazy_flags();
        EXPECT_EQ(state.flags.rflags, oracle.flags.rflags);
    }
}

// Intel SDM Vol. 2A INC/DEC: OF, SF, ZF, AF and PF from the result, CF
// unchanged. The IR takes their N/Z/V straight off the ADDS/SUBS #1 for
// signed and zero readers; a CF reader must still see the carry merged in
// from the earlier producer, not ARM C.
TEST_F(IrX64CarryChain, IncDecFlagsServeEveryReaderAndKeepTheCarry) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    const std::vector<std::vector<u8>> producers{
        {0x01, 0xD1},                // add ecx,edx
        {0x83, 0xDE, 0x00},          // sbb esi,0
    };
    const std::vector<std::vector<u8>> steps{
        {0x48, 0xFF, 0xCB},          // dec rbx
        {0xFF, 0xC3},                // inc ebx
        {0xFF, 0xCB},                // dec ebx
        {0x48, 0xFF, 0xC3},          // inc rbx
    };
    const std::vector<std::vector<u8>> readers{
        {0x0F, 0x95, 0xC2},          // setnz dl
        {0x0F, 0x9C, 0xC2},          // setl dl
        {0x0F, 0x9E, 0xC2},          // setle dl
        {0x0F, 0x98, 0xC2},          // sets dl
        {0x0F, 0x90, 0xC2},          // seto dl
        {0x0F, 0x92, 0xC2},          // setb dl
        {0x0F, 0x97, 0xC2},          // seta dl
    };
    for (const auto& p : producers) {
        for (const auto& s : steps) {
            for (const auto& r : readers) {
                SCOPED_TRACE(testing::Message() << std::hex << unsigned(s[s.size() - 1])
                             << " reader " << unsigned(r[1]));
                expect_alu_matches_interpreter(cat({&p, &s, &r}), 3);
            }
        }
    }
}

// SHLD's flags die at the SHR (Intel SDM Vol. 2: SHR with a nonzero count
// writes CF, OF, SF, ZF and PF). The dead record must not keep SHLD's
// original destination, or the OF sign change built from it, alive: with
// them the three-register pool overflows and the block spills.
TEST_F(IrX64CarryChain, DeadShldRecordKeepsNoOperandAlive) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    const std::vector<u8> body{0x0F, 0xB6, 0x30,          // movzx esi,byte [rax]
                               0x89, 0xD1,                // mov ecx,edx
                               0x0F, 0xA4, 0xFA, 0x08,    // shld edx,edi,8
                               0xC1, 0xE9, 0x18,          // shr ecx,24
                               0x31, 0xF1};               // xor ecx,esi
    auto* native = compile(body);
    ASSERT_NE(native, nullptr);
    const u32 spill_base = arm64::ldr_x_uoff(REG_SCRATCH3, REG_JIT_CTX,
                                             JIT_CTX_OFF_IR_SPILL_FRAME >> 3);
    const auto* words = static_cast<const u32*>(native->code);
    const usize n = native->code_size / sizeof(u32);
    EXPECT_EQ(std::count(words, words + n, spill_base), 0)
        << ir::print(jit->ir_block());
    expect_alu_matches_interpreter(body, 5);
}

// The bench loop's tail: `dec rbx ; setnz` reads ZF off the SUBS itself.
TEST_F(IrX64CarryChain, DecFeedsItsZeroReaderFromTheSubs) {
    ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite));
    const std::vector<u8> body{0x01, 0xD1,          // add ecx,edx
                               0x48, 0xFF, 0xCB,    // dec rbx
                               0x0F, 0x95, 0xC2};   // setnz dl
    auto* native = compile(body);
    ASSERT_NE(native, nullptr);
    const auto* words = static_cast<const u32*>(native->code);
    const usize n = native->code_size / sizeof(u32);
    u32 subs_imm1 = 0;
    for (usize i = 0; i < n; ++i) {
        // SUBS Xd, Xn, #1 (sf=1, op=1, S=1, sh=0, imm12=1).
        if ((words[i] & 0xFFFFFC00u) == 0xF1000400u) ++subs_imm1;
    }
    EXPECT_EQ(subs_imm1, 1u) << ir::print(jit->ir_block());
    expect_alu_matches_interpreter(body, 3);
}

}  // namespace
#endif
