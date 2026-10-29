// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_jit_ir.cpp
/// @brief Stage 1 block IR: the verifier is the artifact under test.
///
/// The IR exists so that the dataflow facts the stencil compiler carries in
/// mutable members become checkable properties of a node.
/// A checkable property is only worth
/// anything if the check actually rejects the malformed shape, so nearly every
/// test here builds a block that *must not* verify.
///
/// No `JitCompiler` is constructed anywhere in this file, and none can be:
/// the IR deliberately has no dependency on it (plan §1, Seam A).

#include <gtest/gtest.h>

#include <string>

#include "core/cpu/jit/ir/jit_ir.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit::ir;

namespace {

constexpr u64 kRip0 = 0x00401000;
constexpr u64 kRip1 = 0x00401003;
constexpr u64 kRip2 = 0x00401006;

constexpr GuestReg kEax{0, Type::I32};
constexpr GuestReg kEcx{1, Type::I32};
constexpr GuestReg kAl{0, Type::I8};

IrBlock make_block() {
    IrBlock block;
    block.entry_rip = kRip0;
    block.is_64bit = false;
    return block;
}

/// A published lazy record, the architecturally authoritative representation.
FlagDef record_def(FlagState::FlagOp op) {
    FlagDef def;
    def.repr = FlagRepr::InCpuStateRecord;
    def.record = op;
    return def;
}

FlagDef dead_def() {
    FlagDef def;
    def.repr = FlagRepr::Dead;
    return def;
}

FlagDef pstate_def(NzcvKind kind) {
    FlagDef def;
    def.repr = FlagRepr::InPstate;
    def.nzcv = kind;
    return def;
}

/// `verify` with the reason discarded; use `verify(block, &why)` directly when
/// the message matters.
bool ok(const IrBlock& block) {
    std::string why;
    const bool result = verify(block, &why);
    if (!result) {
        ADD_FAILURE() << "expected a valid block, verifier said: " << why
                      << "\n" << print(block);
    }
    return result;
}

}  // namespace

// =============================================================================
// Construction
// =============================================================================

// `add eax, ecx` as the IR sees it: two reads, one add, one flag definition,
// one write back to the pin. Guest registers never become plain SSA values --
// plan §3.2 Option A keeps the existing pin ABI as their only home, because a
// guest register living somewhere the fault handler cannot see is silently
// lost inside an SEH handler.
TEST(JitIr, BuildsRegisterAluBlock) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId eax = b.gpr_read(kEax);
    const ValueId ecx = b.gpr_read(kEcx);
    const ValueId sum = b.binary(Op::Add, Type::I32, eax, ecx);
    b.def_flags(record_def(FlagState::FlagOp::Add32), ecx, sum);
    b.gpr_write(kEax, sum);

    EXPECT_EQ(block.size(), 5u);
    EXPECT_EQ(block.type_of(sum), Type::I32);
    EXPECT_TRUE(ok(block));
}

TEST(JitIr, ClearKeepsCapacityAndResetsValueNumbering) {
    IrBlock block = make_block();
    block.reserve(32);
    IrBuilder b(block);
    b.set_rip(kRip0);
    b.gpr_read(kEax);
    const auto capacity = block.insts.capacity();

    block.clear();
    EXPECT_TRUE(block.empty());
    EXPECT_EQ(block.next_value, 0u);
    // The intended lifecycle is one long-lived block reused per compile; if
    // clear() dropped capacity the steady state would allocate on every block.
    EXPECT_EQ(block.insts.capacity(), capacity);
}

TEST(JitIr, LeaWithoutIndexVerifies) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId base = b.gpr_read(kEax);
    const ValueId idx = b.gpr_read(kEcx);
    b.lea(Type::I32, base, idx, 4, -8);
    b.lea(Type::I32, base, kInvalidValue, 1, 16);
    EXPECT_TRUE(ok(block));
}

// =============================================================================
// SSA violations
// =============================================================================

TEST(JitIr, RejectsUseBeforeDef) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId eax = b.gpr_read(kEax);
    b.binary(Op::Add, Type::I32, eax, eax);
    // Rewrite the second operand to a value that is only defined later.
    const ValueId later = block.next_value++;
    block.insts[1].operands[1] = Operand::value(later, Type::I32);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("before it is defined"), std::string::npos) << why;
}

TEST(JitIr, RejectsValueDefinedTwice) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    b.gpr_read(kEax);
    b.gpr_read(kEcx);
    // Two ops now define %0. Value numbering is independent of instruction
    // indices precisely so this stays representable and therefore testable.
    block.insts[1].result = block.insts[0].result;

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("single-assignment"), std::string::npos) << why;
}

TEST(JitIr, RejectsUseOfUnnumberedValue) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId eax = b.gpr_read(kEax);
    b.binary(Op::Add, Type::I32, eax, eax);
    block.insts[1].operands[1] = Operand::value(999, Type::I32);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("never numbered"), std::string::npos) << why;
}

// =============================================================================
// Type discipline -- there is no implicit width coercion anywhere
// =============================================================================

TEST(JitIr, RejectsMixedWidthBinary) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId eax = b.gpr_read(kEax);
    const ValueId al = b.gpr_read(kAl);
    // add i32, i8 -- exactly the coercion that makes x86 partial-register
    // writes a correctness minefield.
    Inst bad;
    bad.op = Op::Add;
    bad.type = Type::I32;
    bad.guest_rip = kRip0;
    bad.operand_count = 2;
    bad.operands[0] = Operand::value(eax, Type::I32);
    bad.operands[1] = Operand::value(al, Type::I8);
    bad.result = block.next_value++;
    block.insts.push_back(bad);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("implicit width coercion"), std::string::npos) << why;
}

TEST(JitIr, RejectsOperandTypeDisagreeingWithDefinition) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId al = b.gpr_read(kAl);
    b.binary(Op::Add, Type::I32, al, al);  // claims %0 is i32; it is i8

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("but it is defined as"), std::string::npos) << why;
}

TEST(JitIr, RejectsNarrowingExtension) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId eax = b.gpr_read(kEax);
    b.zext(Type::I8, eax);  // "widening" i32 -> i8

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("does not widen"), std::string::npos) << why;
}

TEST(JitIr, AcceptsExplicitWidening) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId al = b.gpr_read(kAl);
    const ValueId wide = b.zext(Type::I32, al);
    b.gpr_write(kEax, wide);
    EXPECT_TRUE(ok(block));
}

TEST(JitIr, RejectsGprWriteOfWrongWidth) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId al = b.gpr_read(kAl);
    Inst bad;
    bad.op = Op::GprWrite;
    bad.type = Type::Void;
    bad.guest_rip = kRip0;
    bad.operand_count = 2;
    bad.operands[0] = Operand::gpr(kEax);
    bad.operands[1] = Operand::value(al, Type::I8);
    block.insts.push_back(bad);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("widen or narrow explicitly"), std::string::npos) << why;
}

TEST(JitIr, RejectsConstantThatDoesNotFitItsType) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    b.constant(Type::I8, 0x1234);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("does not fit"), std::string::npos) << why;
}

TEST(JitIr, AcceptsSignExtendedConstant) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    b.constant(Type::I32, 0xFFFF'FFFF'FFFF'FFFFull);  // -1 as i32
    EXPECT_TRUE(ok(block));
}

TEST(JitIr, RejectsShiftCountAtOrAboveWidth) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId eax = b.gpr_read(kEax);
    // x86 masks the count by 31 first; an unmasked 32 here would silently mean
    // a different instruction. Masking must be explicit in the lowering.
    b.shift_imm(Op::Shl, Type::I32, eax, 32);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("mask the count explicitly"), std::string::npos) << why;
}

TEST(JitIr, RejectsRuntimeShiftCountThatIsNotI8) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId eax = b.gpr_read(kEax);
    const ValueId ecx = b.gpr_read(kEcx);
    b.shift_reg(Op::Shl, Type::I32, eax, ecx);
    // Typed honestly, so the shift's own rule is what rejects it rather than
    // the generic use-type check. A 32-bit shift count is a category error:
    // x86 masks the count to 5 bits and the masking must be an explicit op.
    block.insts[2].operands[1] = Operand::value(ecx, Type::I32);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("count must be i8"), std::string::npos) << why;
}

TEST(JitIr, RejectsRegisterOutsideThirtyTwoBitFile) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    b.gpr_read(GuestReg{9, Type::I32});

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("32-bit block"), std::string::npos) << why;
}

// =============================================================================
// guest_rip is mandatory
// =============================================================================

TEST(JitIr, RejectsMissingGuestRip) {
    IrBlock block = make_block();
    IrBuilder b(block);
    // set_rip() deliberately not called: the builder stamps kInvalidRip and
    // the verifier must catch it. An op with no owning guest instruction
    // cannot be attributed by the host-PC -> guest-RIP side table at a fault.
    b.gpr_read(kEax);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("no guest_rip"), std::string::npos) << why;
}

TEST(JitIr, RejectsOpsThatLeaveTheirRipInterval) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip1);
    const ValueId eax = b.gpr_read(kEax);
    b.set_rip(kRip0);  // back into the previous guest instruction
    b.gpr_write(kEcx, eax);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("ascending RIP intervals"), std::string::npos) << why;
}

TEST(JitIr, RejectsFaultStateWithoutMayFault) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    b.gpr_read(kEax);
    block.insts[0].fault_state = FaultState::RipOnly;  // but may_fault is false

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("cannot fault but declares"), std::string::npos) << why;
}

// =============================================================================
// The flag representation lattice
// =============================================================================

TEST(JitIr, RejectsDeadFlagDefThatStillClaimsAHome) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId eax = b.gpr_read(kEax);
    FlagDef def = dead_def();
    def.nzcv = NzcvKind::Sub;
    b.def_flags(def, eax);
    b.def_flags(record_def(FlagState::FlagOp::Add32), eax);  // killer

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("still claims a home"), std::string::npos) << why;
}

TEST(JitIr, RejectsPstateFlagDefWithNoKind) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId eax = b.gpr_read(kEax);
    b.def_flags(pstate_def(NzcvKind::None), eax);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("names no NZCV kind"), std::string::npos) << why;
}

TEST(JitIr, AcceptsRecordPlusNzcvFastPathAnnotation) {
    // A fusable compare publishes the lazy record AND leaves NZCV valid. The
    // record is the architectural home; the NZCV kind is the extra fact a Jcc
    // fuses off (`m_flags.nzcv` / `m_flags.incoming_nzcv` today).
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId eax = b.gpr_read(kEax);
    FlagDef def = record_def(FlagState::FlagOp::Sub32);
    def.nzcv = NzcvKind::Sub;
    b.def_flags(def, eax);
    EXPECT_TRUE(ok(block));
}

TEST(JitIr, RejectsDeadFlagDefWithNoKiller) {
    // "Dead" means "overwritten before any consumer", not "nobody used it in
    // this block". A live-out flag definition must have a home.
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId eax = b.gpr_read(kEax);
    b.def_flags(dead_def(), eax);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("live-out"), std::string::npos) << why;
}

TEST(JitIr, RejectsKeepOverlayOnALiveDefinition) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId eax = b.gpr_read(kEax);
    FlagDef def = record_def(FlagState::FlagOp::Add32);
    def.keep_overlay = true;
    b.def_flags(def, eax);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("keep_overlay"), std::string::npos) << why;
}

// =============================================================================
// The may_fault / flag-liveness invariant (plan §3.3)
//
// This is the one the hand-written scan gets wrong today:
// `compute_omit_lazy_flags` (flag_liveness.cpp:604-609) crosses instructions
// `classify()` calls Neutral, and `MOV [mem], eax` is one of them, so the
// guard `pending_flags_dead_across` applies at :355-362 is missing on the
// general path.
// =============================================================================

/// `cmp eax, imm ; <faulting op> ; add eax, 1` -- the exact shape from plan §3.
/// `repr` and the faulting op's `fault_state` are the two knobs the tests turn.
IrBlock cmp_fault_add(FlagDef cmp_flags, FaultState fault_state) {
    IrBlock block = make_block();
    IrBuilder b(block);

    b.set_rip(kRip0);  // cmp eax, imm
    const ValueId eax = b.gpr_read(kEax);
    const ValueId imm = b.constant(Type::I32, 7);
    const ValueId diff = b.binary(Op::Sub, Type::I32, eax, imm);
    b.def_flags(cmp_flags, imm, diff);

    b.set_rip(kRip1);  // a store: Stage 2's op, modelled here by the hook
    b.gpr_read(kEcx);
    b.mark_may_fault(fault_state);

    b.set_rip(kRip2);  // add eax, 1 -- redefines every flag, so it is the killer
    const ValueId one = b.constant(Type::I32, 1);
    const ValueId sum = b.binary(Op::Add, Type::I32, eax, one);
    b.def_flags(record_def(FlagState::FlagOp::Add32), one, sum);
    b.gpr_write(kEax, sum);
    return block;
}

TEST(JitIr, RejectsDeadFlagDefAcrossAFaultingOp) {
    IrBlock block = cmp_fault_add(dead_def(), FaultState::RipAndFlags);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("may fault before its killer"), std::string::npos) << why;
}

TEST(JitIr, RejectsPstateOnlyFlagDefAcrossAFaultingOp) {
    // The signal handler does not persist host NZCV, so PSTATE is not a home
    // that survives a fault no matter what the faulting op declares.
    IrBlock block = cmp_fault_add(pstate_def(NzcvKind::Sub),
                                  FaultState::RipAndFlags);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("does not persist NZCV"), std::string::npos) << why;
}

TEST(JitIr, RejectsFaultingOpThatUnderdeclaresItsFlagState) {
    // The record IS published, so the flags are recoverable -- but the op
    // claims it only needs RIP. Declaring less than is required is how a later
    // pass talks itself into dropping the record.
    IrBlock block = cmp_fault_add(record_def(FlagState::FlagOp::Sub32),
                                  FaultState::RipOnly);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("must declare rip+flags"), std::string::npos) << why;
}

TEST(JitIr, AcceptsPublishedRecordAcrossAFaultingOp) {
    // The same block, correctly described: the CMP's record is published and
    // the faulting op declares rip+flags. This is what the stencil compiler
    // must do and currently does not always do.
    EXPECT_TRUE(ok(cmp_fault_add(record_def(FlagState::FlagOp::Sub32),
                                 FaultState::RipAndFlags)));
}

TEST(JitIr, AcceptsDeadFlagDefWhenNothingBetweenCanFault) {
    // `cmp eax, imm ; add eax, 1` with no faulting op in between: the CMP's
    // record is genuinely unobservable and omitting it is the win the whole
    // liveness pass exists for.
    IrBlock block = make_block();
    IrBuilder b(block);

    b.set_rip(kRip0);
    const ValueId eax = b.gpr_read(kEax);
    const ValueId imm = b.constant(Type::I32, 7);
    const ValueId diff = b.binary(Op::Sub, Type::I32, eax, imm);
    b.def_flags(dead_def(), imm, diff);

    b.set_rip(kRip1);
    const ValueId one = b.constant(Type::I32, 1);
    const ValueId sum = b.binary(Op::Add, Type::I32, eax, one);
    b.def_flags(record_def(FlagState::FlagOp::Add32), one, sum);
    b.gpr_write(kEax, sum);

    EXPECT_TRUE(ok(block));
}

// =============================================================================
// Printer
// =============================================================================

TEST(JitIr, PrintsExpectedText) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId eax = b.gpr_read(kEax);
    const ValueId imm = b.constant(Type::I32, 0x10);
    const ValueId sum = b.binary(Op::Add, Type::I32, eax, imm);
    FlagDef def = record_def(FlagState::FlagOp::Add32);
    def.nzcv = NzcvKind::Add;
    b.def_flags(def, imm, sum);
    b.gpr_write(kEax, sum);
    ASSERT_TRUE(ok(block));

    const std::string text = print(block);
    EXPECT_EQ(text,
              "block entry=0x401000 bits=32\n"
              "  %0:i32 = gpr.read eax  ; rip=0x401000\n"
              "  %1:i32 = const 0x10  ; rip=0x401000\n"
              "  %2:i32 = add %0, %1  ; rip=0x401000\n"
              "  %3:flags = defflags %1, %2  ; rip=0x401000 repr=record "
              "nzcv=add record=add32\n"
              "  gpr.write eax, %2  ; rip=0x401000\n");
}

TEST(JitIr, PrintIsStableAcrossCallsAndCopies) {
    IrBlock block = cmp_fault_add(record_def(FlagState::FlagOp::Sub32),
                                  FaultState::RipAndFlags);
    ASSERT_TRUE(ok(block));

    const std::string first = print(block);
    EXPECT_EQ(first, print(block));

    // A copied block prints identically: nothing in the IR is identity- or
    // address-dependent, which is what makes printed IR usable as a golden
    // artifact in later differential tests.
    const IrBlock copy = block;
    EXPECT_EQ(first, print(copy));

    // The faulting op's declaration is visible in the dump -- a decline reason
    // is only useful if the state it names can be read back out.
    EXPECT_NE(first.find("may_fault=rip+flags"), std::string::npos) << first;
}

TEST(JitIr, PrintsLeaOperandsReadably) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId base = b.gpr_read(kEax);
    const ValueId idx = b.gpr_read(kEcx);
    b.lea(Type::I32, base, idx, 4, -8);
    ASSERT_TRUE(ok(block));

    const std::string text = print_inst(block, 2);
    EXPECT_EQ(text, "%2:i32 = lea %0, %1, 4, -8  ; rip=0x401000");
}

TEST(JitIr, VerifierReasonNamesTheOffendingOp) {
    // The decline reason is what VITREO_JIT_IR_STATS reports (plan §4); a
    // reason that does not identify the op is not actionable.
    IrBlock block = cmp_fault_add(dead_def(), FaultState::RipAndFlags);
    std::string why;
    ASSERT_FALSE(verify(block, &why));
    // op 3 is the CMP's flag definition; op 4 is the faulting op between it
    // and its killer.
    EXPECT_NE(why.find("op 3"), std::string::npos) << why;
    EXPECT_NE(why.find("op 4"), std::string::npos) << why;
}

// =============================================================================
// Memory ops
// =============================================================================

TEST(JitIr, LoadAndStoreVerify) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId addr = b.gpr_read(kEcx);
    const ValueId v = b.load(Type::I32, addr, FaultState::RipOnly);
    b.gpr_write(kEax, v);
    b.set_rip(kRip1);
    const ValueId addr2 = b.gpr_read(kEcx);
    const ValueId val = b.gpr_read(kEax);
    b.store(Type::I32, addr2, val, FaultState::RipOnly);

    std::string why;
    EXPECT_TRUE(verify(block, &why)) << why;
}

// THE address-width rule. Vitreo has a recorded FAMILY of bugs (four
// instances) where a 64-bit effective address was marshalled through a 32-bit
// move and silently truncated — invisible below 4 GB, which is where oracle
// tests run, so every one of them survived the suite and surfaced as a wild
// pointer in a real guest. In this IR the mistake is not representable: the
// address operand carries its type and the verifier demands the block's
// pointer width.
TEST(JitIr, RejectsATruncatedAddressInASixtyFourBitBlock) {
    IrBlock block = make_block();
    block.is_64bit = true;
    IrBuilder b(block);
    b.set_rip(kRip0);
    // A 32-bit address value in a 64-bit block: exactly the truncation.
    const ValueId narrow = b.constant(Type::I32, 0x1000);
    (void)b.load(Type::I32, narrow, FaultState::RipOnly);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("address"), std::string::npos) << why;
}

TEST(JitIr, RejectsAWideAddressInAThirtyTwoBitBlock) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId wide = b.constant(Type::I64, 0x1000);
    (void)b.load(Type::I32, wide, FaultState::RipOnly);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("address"), std::string::npos) << why;
}

TEST(JitIr, RejectsStoreValueDisagreeingWithAccessWidth) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId addr = b.gpr_read(kEcx);
    const ValueId val = b.gpr_read(kEax);          // I32
    b.store(Type::I32, addr, val, FaultState::RipOnly);
    block.insts.back().mem.width = Type::I16;      // now disagrees

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("width"), std::string::npos) << why;
}

// A guest access with no fault path does not exist. The builder cannot make
// one, so the test has to reach in and clear the flag — which is the point:
// the verifier is what catches a hand-built or pass-mangled node.
TEST(JitIr, RejectsAMemoryOpThatClaimsItCannotFault) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId addr = b.gpr_read(kEcx);
    (void)b.load(Type::I32, addr, FaultState::RipOnly);
    block.insts.back().may_fault = false;
    block.insts.back().fault_state = FaultState::None;

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("may_fault"), std::string::npos) << why;
}

// The flag/fault invariant, now with a REAL faulting op rather than the
// synthetic `mark_may_fault` stand-in. A published record survives a load only
// if the load declares that it needs flags too.
TEST(JitIr, LoadWithALiveFlagDefMustDeclareRipAndFlags) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId a = b.gpr_read(kEax);
    const ValueId c = b.gpr_read(kEcx);
    const ValueId r = b.binary(Op::Sub, Type::I32, a, c);
    b.def_flags(FlagDef{FlagRepr::InCpuStateRecord, NzcvKind::None,
                        FlagState::FlagOp::Sub32, false, false}, c, r);

    b.set_rip(kRip1);
    const ValueId addr = b.gpr_read(kEcx);
    (void)b.load(Type::I32, addr, FaultState::RipOnly);  // under-declared

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("rip+flags"), std::string::npos) << why;
}

TEST(JitIr, LoadDeclaringRipAndFlagsAcceptsALiveFlagDef) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId a = b.gpr_read(kEax);
    const ValueId c = b.gpr_read(kEcx);
    const ValueId r = b.binary(Op::Sub, Type::I32, a, c);
    b.def_flags(FlagDef{FlagRepr::InCpuStateRecord, NzcvKind::None,
                        FlagState::FlagOp::Sub32, false, false}, c, r);

    b.set_rip(kRip1);
    const ValueId addr = b.gpr_read(kEcx);
    (void)b.load(Type::I32, addr, FaultState::RipAndFlags);

    std::string why;
    EXPECT_TRUE(verify(block, &why)) << why;
}

TEST(JitIr, PrintsMemoryMetadata) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId addr = b.gpr_read(kEcx);
    (void)b.load(Type::I32, addr, FaultState::RipOnly);
    const std::string text = print(block);
    EXPECT_NE(text.find("load"), std::string::npos) << text;
    EXPECT_NE(text.find("guest:i32"), std::string::npos) << text;
}

// =============================================================================
// Terminators
// =============================================================================

TEST(JitIr, JccAndJmpVerify) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId a = b.gpr_read(kEax);
    const ValueId c = b.gpr_read(kEcx);
    const ValueId r = b.binary(Op::Sub, Type::I32, a, c);
    const ValueId f = b.def_flags(
        FlagDef{FlagRepr::InCpuStateRecord, NzcvKind::None,
                FlagState::FlagOp::Sub32, false, false}, c, r);
    b.set_rip(kRip1);
    b.jcc(0x4, f, 0x500, kRip2);

    std::string why;
    EXPECT_TRUE(verify(block, &why)) << why;

    IrBlock jmp_block = make_block();
    IrBuilder jb(jmp_block);
    jb.set_rip(kRip0);
    jb.jmp(0x500);
    EXPECT_TRUE(verify(jmp_block, &why)) << why;
}

// The rule that keeps the flag-demand pass sound: a terminator ends its block.
// Anything after it is unreachable, and — more importantly — a flag definition
// after a branch would mean a definition could be killed ACROSS one, which the
// pass's linear killer scan cannot reason about.
TEST(JitIr, RejectsOpsAfterATerminator) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    b.jmp(0x500);
    b.set_rip(kRip1);
    (void)b.gpr_read(kEax);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("terminator"), std::string::npos) << why;
}

TEST(JitIr, RejectsTwoTerminators) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    b.jmp(0x500);
    b.set_rip(kRip1);
    b.jmp(0x600);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
}

TEST(JitIr, RejectsJccWhoseConditionIsNotAFlagsValue) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId v = b.gpr_read(kEax);   // an i32, not flags
    b.jcc(0x4, v, 0x500, kRip1);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
    EXPECT_NE(why.find("flags"), std::string::npos) << why;
}

TEST(JitIr, RejectsAJccWithNoFallThrough) {
    IrBlock block = make_block();
    IrBuilder b(block);
    b.set_rip(kRip0);
    const ValueId a = b.gpr_read(kEax);
    const ValueId c = b.gpr_read(kEcx);
    const ValueId r = b.binary(Op::Sub, Type::I32, a, c);
    const ValueId f = b.def_flags(
        FlagDef{FlagRepr::InCpuStateRecord, NzcvKind::None,
                FlagState::FlagOp::Sub32, false, false}, c, r);
    b.jcc(0x4, f, 0x500, kInvalidRip);

    std::string why;
    EXPECT_FALSE(verify(block, &why));
}
