// SPDX-License-Identifier: LGPL-2.1-only
// WS2: loop-idiom matcher -- structure, data flow, exits, kinds, near misses.

#include "test_jit_loop_idiom_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::loop_idiom;
using namespace vitreo::cpu::jit::loop_idiom::test;
using namespace vitreo::cpu::jit::loop_idiom::test::catalogue;
using Form = InlineGate::Form;

namespace {

constexpr u8 EAX = 0, ECX = 1, EDX = 2, ESI = 6, EDI = 7;

struct Expect {
    const char* name;
    std::span<const u8> code;
    bool is64;
    Kind kind;
    u8 elem_log2;
    u8 n_loads;
    bool has_store;
    u8 n_ind;
    bool has_ind_exit;
    u8 n_data;
    Form gate;
    bool dir_down;
};

const Expect kCatalogue[] = {
    {"F1", kF1, false, Kind::Fill, 0, 0, true, 2, true, 0, Form::CountDown, false},
    {"F2", kF2, false, Kind::Fill, 2, 0, true, 1, true, 0, Form::UpToBound, false},
    {"F3", kF3, false, Kind::Fill, 1, 0, true, 1, true, 0, Form::NegIndexUp, false},
    {"F4", kF4, true, Kind::Fill, 0, 0, true, 1, true, 0, Form::UpToBound, false},
    {"F5", kF5, true, Kind::Fill, 3, 0, true, 1, true, 0, Form::UpToBound, false},
    {"F6", kF6, true, Kind::Fill, 4, 0, true, 1, true, 0, Form::UpToBound, false},
    {"F7", kF7, true, Kind::Fill, 4, 0, true, 1, true, 0, Form::UpToBound, false},
    {"F8", kF8, false, Kind::Fill, 0, 0, true, 2, true, 0, Form::CountDown, false},
    {"F9", kF9, true, Kind::Fill, 2, 0, true, 2, true, 0, Form::CountDown, false},
    {"C1", kC1, false, Kind::Copy, 0, 1, true, 3, true, 0, Form::CountDown, false},
    {"C2", kC2, false, Kind::Copy, 0, 1, true, 3, true, 0, Form::CountDown, false},
    {"C3", kC3, false, Kind::Copy, 2, 1, true, 1, true, 0, Form::UpToBound, false},
    {"C4", kC4, false, Kind::Copy, 0, 1, true, 3, true, 0, Form::CountDown, true},
    {"C5", kC5, false, Kind::Copy, 0, 1, true, 2, false, 1, Form::None, false},
    {"C6", kC6, false, Kind::Copy, 0, 1, true, 3, true, 0, Form::CountDown, false},
    {"C7", kC7, true, Kind::Copy, 0, 1, true, 1, true, 0, Form::NegIndexUp, false},
    {"C8", kC8, true, Kind::Copy, 3, 1, true, 1, true, 0, Form::UpToBound, false},
    {"C9", kC9, true, Kind::Copy, 4, 1, true, 1, true, 0, Form::UpToBound, false},
    {"C10", kC10, true, Kind::Copy, 4, 1, true, 1, true, 0, Form::UpToBound, false},
    {"C11", kC11, true, Kind::Copy, 2, 1, true, 3, true, 0, Form::CountDown, false},
    {"C12", kC12, true, Kind::Copy, 1, 1, true, 1, false, 1, Form::None, false},
    {"S1", kS1, false, Kind::Scan, 0, 1, false, 1, false, 1, Form::None, false},
    {"S2", kS2, false, Kind::Scan, 0, 1, false, 1, false, 1, Form::None, false},
    {"S3", kS3, false, Kind::Scan, 0, 1, false, 2, true, 1, Form::CountDown, false},
    {"S3h", kS3h, false, Kind::Scan, 0, 1, false, 2, true, 1, Form::CountDown, false},
    {"S4", kS4, true, Kind::Scan, 0, 1, false, 1, false, 1, Form::None, false},
    {"S5", kS5, true, Kind::Scan, 1, 1, false, 1, false, 1, Form::None, false},
    {"S6", kS6, true, Kind::Scan, 4, 1, false, 1, false, 1, Form::None, false},
    {"S6w", kS6w, true, Kind::Scan, 4, 1, false, 1, false, 1, Form::None, false},
    {"S6d", kS6d, true, Kind::Scan, 4, 1, false, 1, false, 1, Form::None, false},
    {"S7", kS7, true, Kind::Scan, 0, 1, false, 2, true, 1, Form::CountDown, false},
    {"M1", kM1, false, Kind::Compare, 0, 2, false, 1, true, 1, Form::UpToBound, false},
    {"M2", kM2, false, Kind::Compare, 0, 2, false, 2, false, 2, Form::None, false},
    {"M3", kM3, true, Kind::Compare, 0, 2, false, 1, true, 1, Form::UpToBound, false},
    {"M4", kM4, true, Kind::Compare, 3, 2, false, 1, true, 1, Form::UpToBound, false},
    {"M5", kM5, true, Kind::Compare, 1, 2, false, 1, false, 2, Form::None, false},
    {"L1", kL1, false, Kind::Fill, 0, 0, true, 2, true, 0, Form::CountDown, false},
    {"G1", kG1, false, Kind::Fill, 0, 0, true, 1, true, 0, Form::None, false},
};

}  // namespace

TEST(LoopIdiomMatcher, ClassifiesCatalogue) {
    for (const Expect& e : kCatalogue) {
        SCOPED_TRACE(e.name);
        const MatchResult r = match_bytes(e.code, e.is64);
        ASSERT_TRUE(r.match.has_value()) << reject_reason_name(r.reason);
        const Desc& d = r.match->desc;
        EXPECT_EQ(d.kind, e.kind);
        EXPECT_EQ(d.elem_log2, e.elem_log2);
        EXPECT_EQ(d.n_loads, e.n_loads);
        EXPECT_EQ(d.has_store, e.has_store);
        EXPECT_EQ(d.n_ind, e.n_ind);
        EXPECT_EQ(d.has_ind_exit, e.has_ind_exit);
        EXPECT_EQ(d.n_data, e.n_data);
        EXPECT_EQ(d.dir_down, e.dir_down);
        EXPECT_EQ(d.is64, e.is64);
        EXPECT_EQ(d.vector, e.elem_log2 == 4);
        EXPECT_EQ(d.loop_rip, kCodeBase);
        EXPECT_EQ(r.match->gate.form, e.gate);
        EXPECT_TRUE(pack(d).has_value());
    }
}

TEST(LoopIdiomMatcher, ExactDescriptors) {
    {
        SCOPED_TRACE("F1");
        const auto r = match_bytes(kF1, false);
        ASSERT_TRUE(r.match);
        const Desc& d = r.match->desc;
        EXPECT_EQ(d.streams[0], (Stream{EDI, kNoReg, 0, 0, false}));
        EXPECT_EQ(d.ind[0], (Induction{EDI, 1, false}));
        EXPECT_EQ(d.ind[1], (Induction{ECX, -1, false}));
        EXPECT_EQ(d.ind_exit, (IndExit{1, Cond::NZ, ValueSrc::Imm, 0, 0, true, 1, false}));
        EXPECT_EQ(d.fill, (Value{ValueSrc::Gpr, EAX, false, 0}));
        EXPECT_EQ(r.match->gate.reg, ECX);
    }
    {
        SCOPED_TRACE("F2");
        const auto r = match_bytes(kF2, false);
        ASSERT_TRUE(r.match);
        const Desc& d = r.match->desc;
        EXPECT_EQ(d.streams[0], (Stream{EDI, EAX, 2, 0, false}));
        EXPECT_EQ(d.ind_exit, (IndExit{0, Cond::B, ValueSrc::Gpr, ECX, 0, true, 1, false}));
        EXPECT_EQ(d.fill, (Value{ValueSrc::Gpr, EDX, false, 0}));
        EXPECT_EQ(r.match->gate.bound_reg, ECX);
        EXPECT_FALSE(r.match->gate.bound_is_imm);
    }
    {
        SCOPED_TRACE("F3");
        const auto r = match_bytes(kF3, false);
        ASSERT_TRUE(r.match);
        EXPECT_EQ(r.match->desc.fill, (Value{ValueSrc::Imm, 0, false, 0x2020}));
        EXPECT_EQ(r.match->desc.streams[0], (Stream{EDI, ECX, 1, 0, false}));
    }
    {
        SCOPED_TRACE("F4: induction on the right of CMP");
        const auto r = match_bytes(kF4, true);
        ASSERT_TRUE(r.match);
        EXPECT_EQ(r.match->desc.ind_exit,
                  (IndExit{0, Cond::NZ, ValueSrc::Gpr, EDX, 0, false, 1, true}));
    }
    {
        SCOPED_TRACE("F8: AH source");
        const auto r = match_bytes(kF8, false);
        ASSERT_TRUE(r.match);
        EXPECT_EQ(r.match->desc.fill, (Value{ValueSrc::Gpr, EAX, true, 0}));
    }
    {
        SCOPED_TRACE("F9: 32-bit count in 64-bit mode");
        const auto r = match_bytes(kF9, true);
        ASSERT_TRUE(r.match);
        EXPECT_EQ(r.match->desc.ind[1], (Induction{EDX, -1, false}));
        EXPECT_FALSE(r.match->gate.wide);
    }
    {
        SCOPED_TRACE("C4: update before access folds into disp_eff");
        const auto r = match_bytes(kC4, false);
        ASSERT_TRUE(r.match);
        EXPECT_EQ(r.match->desc.streams[0], (Stream{ESI, kNoReg, 0, -1, false}));
        EXPECT_EQ(r.match->desc.streams[1], (Stream{EDI, kNoReg, 0, -1, false}));
    }
    {
        SCOPED_TRACE("C5");
        const auto r = match_bytes(kC5, false);
        ASSERT_TRUE(r.match);
        EXPECT_EQ(r.match->desc.data[0],
                  (DataExit{0, DataOp::ValueEq, true, ValueSrc::Imm, 0, false, 0, 0}));
    }
    {
        SCOPED_TRACE("C6: LOOP counts ECX");
        const auto r = match_bytes(kC6, false);
        ASSERT_TRUE(r.match);
        EXPECT_EQ(r.match->desc.ind[2], (Induction{ECX, -1, false}));
        EXPECT_EQ(r.match->desc.ind_exit.ind, 2);
        EXPECT_EQ(r.match->gate.reg, ECX);
    }
    {
        SCOPED_TRACE("C10 aligned");
        const auto r = match_bytes(kC10, true);
        ASSERT_TRUE(r.match);
        EXPECT_TRUE(r.match->desc.streams[0].align16);
        EXPECT_TRUE(r.match->desc.streams[1].align16);
        const auto u = match_bytes(kC9, true);
        ASSERT_TRUE(u.match);
        EXPECT_FALSE(u.match->desc.streams[0].align16);
    }
    {
        SCOPED_TRACE("S3h: AH value");
        const auto r = match_bytes(kS3h, false);
        ASSERT_TRUE(r.match);
        EXPECT_EQ(r.match->desc.data[0],
                  (DataExit{0, DataOp::ValueEq, true, ValueSrc::Gpr, EAX, true, 0, 0}));
    }
    {
        SCOPED_TRACE("S4");
        const auto r = match_bytes(kS4, true);
        ASSERT_TRUE(r.match);
        EXPECT_EQ(r.match->desc.streams[0], (Stream{ECX, EAX, 0, 1, false}));
    }
    {
        SCOPED_TRACE("S5");
        const auto r = match_bytes(kS5, true);
        ASSERT_TRUE(r.match);
        EXPECT_EQ(r.match->desc.streams[0], (Stream{EAX, ECX, 1, 2, false}));
    }
    {
        SCOPED_TRACE("S6 lanes");
        for (auto [code, lane] : {std::pair{std::span<const u8>(kS6), 0},
                                  std::pair{std::span<const u8>(kS6w), 1},
                                  std::pair{std::span<const u8>(kS6d), 2}}) {
            const auto r = match_bytes(code, true);
            ASSERT_TRUE(r.match);
            EXPECT_EQ(r.match->desc.data[0],
                      (DataExit{0, DataOp::AnyLaneEq, true, ValueSrc::Xmm, 0, false, 0,
                                static_cast<u8>(lane)}));
        }
    }
    {
        SCOPED_TRACE("S7: SIL with REX");
        const auto r = match_bytes(kS7, true);
        ASSERT_TRUE(r.match);
        EXPECT_EQ(r.match->desc.data[0].val_reg, ESI);
        EXPECT_FALSE(r.match->desc.data[0].val_high8);
    }
    {
        SCOPED_TRACE("M2");
        const auto r = match_bytes(kM2, false);
        ASSERT_TRUE(r.match);
        const Desc& d = r.match->desc;
        EXPECT_EQ(d.streams[0], (Stream{ESI, kNoReg, 0, 0, false}));
        EXPECT_EQ(d.streams[1], (Stream{EDI, kNoReg, 0, 0, false}));
        EXPECT_EQ(d.data[0].op, DataOp::StreamEq);
        EXPECT_FALSE(d.data[0].exit_if_eq);
        EXPECT_EQ(d.data[1],
                  (DataExit{0, DataOp::ValueEq, true, ValueSrc::Imm, 0, false, 0, 0}));
    }
    {
        SCOPED_TRACE("G1: signed bound");
        const auto r = match_bytes(kG1, false);
        ASSERT_TRUE(r.match);
        EXPECT_EQ(r.match->desc.ind_exit,
                  (IndExit{0, Cond::L, ValueSrc::Imm, 0, 0x40, true, 1, false}));
    }
}

TEST(LoopIdiomMatcher, OrderedBitFollowsConfig) {
    const MatchConfig gpr_only{true, false};
    auto c1 = match_bytes(kC1, false, gpr_only);
    ASSERT_TRUE(c1.match);
    EXPECT_TRUE(c1.match->desc.ordered);
    auto c9 = match_bytes(kC9, true, gpr_only);
    ASSERT_TRUE(c9.match);
    EXPECT_FALSE(c9.match->desc.ordered);
    auto c9v = match_bytes(kC9, true, MatchConfig{true, true});
    ASSERT_TRUE(c9v.match);
    EXPECT_TRUE(c9v.match->desc.ordered);
    auto c1off = match_bytes(kC1, false, MatchConfig{false, false});
    ASSERT_TRUE(c1off.match);
    EXPECT_FALSE(c1off.match->desc.ordered);
}

TEST(LoopIdiomMatcher, NearMissRejects) {
    struct Case {
        const char* name;
        std::vector<u8> code;
        bool is64;
        RejectReason why;
    };
    auto v = [](std::span<const u8> s) { return std::vector<u8>(s.begin(), s.end()); };
    std::vector<u8> not_self = v(kF1);
    not_self.back() = 0xFC;  // jnz to start+2
    std::vector<u8> in_body = v(kS3);
    in_body[3] = 0xFC;  // je to start+0 (inside the body, not an exit)
    std::vector<u8> in_body2 = v(kS3);
    in_body2[3] = 0x00;  // je to start+4 (the next instruction)
    const Case cases[] = {
        {"xor inserted", v(kNmXor), false, RejectReason::Opcode},
        {"store stride 2", v(kNmStride), false, RejectReason::Stride},
        {"source stride 2", v(kNmSrcStride), false, RejectReason::Stride},
        {"store reads AL before its load", v(kNmCarried), false, RejectReason::LoopCarried},
        {"adc", v(kNmAdc), false, RejectReason::Opcode},
        // 16-bit addressing in 32-bit mode is refused by the decoder itself,
        // so the block never reaches the matcher; 67h in 64-bit mode does.
        {"67h address size (64-bit mode)", v(kNmAddr32), true, RejectReason::Prefix},
        {"fs: override", v(kNmFs), false, RejectReason::Prefix},
        {"rip-relative store", v(kNmRip), true, RejectReason::RipRelative},
        {"esp base", v(kNmEsp), false, RejectReason::StackReg},
        {"back edge to start+2", not_self, false, RejectReason::NotSelfLoop},
        {"inc; jb (CF unchanged by INC)", v(kNmIncJb), false, RejectReason::CondUndefined},
        {"dec; jg", v(kNmDecJg), false, RejectReason::CondUndefined},
        {"jo", v(kNmJo), false, RejectReason::CondUndefined},
        {"mid exit into the body", in_body, false, RejectReason::InBodyBranch},
        {"mid exit to the next insn", in_body2, false, RejectReason::InBodyBranch},
        {"lock add", v(kNmLock), false, RejectReason::Prefix},
        {"inc cl counter", v(kNmIncCl), false, RejectReason::InductionShape},
        {"byte load, word store", v(kNmWidth), false, RejectReason::Width},
        {"mov al,dl source", v(kNmMovRR), false, RejectReason::Opcode},
        {"two stores", v(kNmTwoStores), false, RejectReason::StreamCount},
        {"fill with a data exit", v(kNmFillData), false, RejectReason::KindShape},
        {"accumulate from memory", v(kNmAddMem), false, RejectReason::Opcode},
        {"store of the induction (iota)", v(kNmIota), false, RejectReason::KindShape},
        {"no exit", v(kNmNoExit), false, RejectReason::NoExit},
        {"rep movsb inside", v(kNmRep), false, RejectReason::Opcode},
        {"qword store, index scale 4", v(kNmQStride), true, RejectReason::Stride},
        {"qword load, dword store", v(kNmQWidth), true, RejectReason::Width},
        {"qword compare, source scale 4", v(kNmQCmpStride), true, RejectReason::Stride},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        const MatchResult r = match_bytes(c.code, c.is64);
        EXPECT_FALSE(r.match.has_value());
        EXPECT_EQ(r.reason, c.why) << "got " << reject_reason_name(r.reason);
    }
}

TEST(LoopIdiomMatcher, ExistingZeroFillShapeStillMatchesOnlyAsZeroFill) {
    // mov byte [eax],0; lea eax,[eax+1]; sub ecx,1; jne: the zero-fill hook
    // takes precedence in the JIT; the WS2 matcher also understands it.
    const u8 code[] = {0xC6, 0x00, 0x00, 0x8D, 0x40, 0x01, 0x83, 0xE9, 0x01, 0x75, 0xF5};
    const auto r = match_bytes(code, false);
    ASSERT_TRUE(r.match);
    EXPECT_EQ(r.match->desc.kind, Kind::Fill);
    EXPECT_EQ(r.match->desc.fill, (Value{ValueSrc::Imm, 0, false, 0}));
}
