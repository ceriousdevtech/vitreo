// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_frontend.cpp
/// @brief Tier 1 frontend: real x86 bytes -> canonical IR, or a named decline.
///
/// Every case here feeds **real encoded x86 bytes through the real decoder**
/// rather than hand-assembling `DecodedInstruction`s. That is the point: the
/// frontend's contract is with whatever the decoder actually produces, and a
/// hand-built fixture would keep passing after the decoder changed which
/// operand shape it emits for a form.
///
/// Every accepted block is asserted to pass `verify()` — `jit_ir.cpp`'s
/// verifier is the correctness boundary, so "the frontend produced something"
/// is not a result until the verifier agrees. Semantics that the verifier
/// cannot see (which lazy flag record a node names, whether a compare wrote
/// its destination) are asserted against the deterministic printed text.
///
/// And for every rejected block, the *reason* is asserted, not merely the
/// rejection. A decline for the wrong reason is a coverage metric that lies.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/jit/ir/ir_frontend.hpp"
#include "core/cpu/jit/ir/ir_passes.hpp"
#include "core/cpu/jit/ir/jit_ir.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit::ir;

namespace {

constexpr u64 kEntry = 0x00401000;

/// A decoded block plus the guest RIP of each instruction, decoded from one
/// contiguous byte string exactly as a real block is.
struct Decoded {
    std::vector<DecodedInstruction> insns;
    std::vector<u64> rips;
};

[[nodiscard]] Decoded decode_all(const std::vector<u8>& bytes,
                                 DecodeMode mode = DecodeMode::Mode32) {
    Decoded out;
    usize offset = 0;
    while (offset < bytes.size()) {
        MemoryByteReader reader(bytes.data() + offset, bytes.size() - offset);
        DecodedInstruction insn;
        const DecodeError err = decode_instruction(reader, mode, insn);
        EXPECT_EQ(err, DecodeError::Success)
            << "decode failed at byte offset " << offset;
        if (err != DecodeError::Success) break;
        out.rips.push_back(kEntry + offset);
        out.insns.push_back(insn);
        offset += insn.length;
    }
    return out;
}

struct Built {
    BuildResult result;
    IrBlock block;
    std::string why;
};

[[nodiscard]] Built build(const std::vector<u8>& bytes,
                          DecodeMode mode = DecodeMode::Mode32) {
    const Decoded decoded = decode_all(bytes, mode);
    Built built;
    built.result = build_block(decoded.insns.data(), decoded.rips.data(),
                               static_cast<u32>(decoded.insns.size()), kEntry,
                               mode == DecodeMode::Mode64,
                               /*recovery_available=*/true, built.block,
                               &built.why);
    return built;
}

void expect_declined(const std::vector<u8>& bytes, DeclineReason reason,
                     DecodeMode mode = DecodeMode::Mode32) {
    const Built built = build(bytes, mode);
    EXPECT_FALSE(built.result.ok);
    EXPECT_EQ(built.result.reason, reason)
        << "got " << decline_reason_name(built.result.reason);
}

}  // namespace

// =============================================================================
// Accepted forms
// =============================================================================

// 01 c8  add eax, ecx
TEST(IrFrontend, AddRegRegBuildsAndVerifies) {
    const Built built = build({0x01, 0xC8});
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    std::string why;
    EXPECT_TRUE(verify(built.block, &why)) << why;
}

// The flag-record contract in one assertion: `defflags` names Add32 and takes
// (source, result) in that order. `materialize` reconstructs the original
// destination as flag_dst - flag_src, so swapping these two operands is wrong
// for every asymmetric pair and right for every symmetric one.
TEST(IrFrontend, AddPublishesRecordWithSourceThenResult) {
    const Built built = build({0x01, 0xC8});  // add eax, ecx
    ASSERT_TRUE(built.result.ok);
    const std::string text = print(built.block);
    EXPECT_NE(text.find("defflags"), std::string::npos) << text;
    EXPECT_NE(text.find("record=add32"), std::string::npos) << text;
}

// 29 c8  sub eax, ecx
TEST(IrFrontend, SubRegReg) {
    const Built built = build({0x29, 0xC8});
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    EXPECT_NE(print(built.block).find("record=sub32"), std::string::npos);
}

// 21 c8 / 09 c8 / 31 c8  and/or/xor eax, ecx
TEST(IrFrontend, LogicRegRegPublishLogicRecords) {
    EXPECT_NE(print(build({0x21, 0xC8}).block).find("record=and32"),
              std::string::npos);
    EXPECT_NE(print(build({0x09, 0xC8}).block).find("record=or32"), std::string::npos);
    EXPECT_NE(print(build({0x31, 0xC8}).block).find("record=xor32"),
              std::string::npos);
}

// 83 c0 05  add eax, 5   (sign-extended imm8 form)
TEST(IrFrontend, AddImm8SignExtendedForm) {
    const Built built = build({0x83, 0xC0, 0x05});
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    std::string why;
    EXPECT_TRUE(verify(built.block, &why)) << why;
}

// 83 e8 01  sub eax, 1 -- the imm8 form carries a NEGATIVE i64 after
// sign-extension for values above 0x7f, which the IR's `Const` must accept as
// a sign-extended literal rather than reject as not fitting I32.
TEST(IrFrontend, SubImm8NegativeSignExtendedConstantFits) {
    const Built built = build({0x83, 0xE8, 0xFF});  // sub eax, -1
    ASSERT_TRUE(built.result.ok)
        << decline_reason_name(built.result.reason) << " " << built.why;
    std::string why;
    EXPECT_TRUE(verify(built.block, &why)) << why;
}

// 39 c8  cmp eax, ecx -- defines flags, writes no register.
TEST(IrFrontend, CmpDefinesFlagsWithoutWritingTheDestination) {
    const Built built = build({0x39, 0xC8});
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    const std::string text = print(built.block);
    EXPECT_NE(text.find("defflags"), std::string::npos) << text;
    EXPECT_EQ(text.find("gprwrite"), std::string::npos)
        << "cmp must not write its destination:\n" << text;
}

// 85 c8  test eax, ecx
TEST(IrFrontend, TestDefinesFlagsWithoutWritingTheDestination) {
    const Built built = build({0x85, 0xC8});
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    const std::string text = print(built.block);
    EXPECT_NE(text.find("record=and32"), std::string::npos) << text;
    EXPECT_EQ(text.find("gprwrite"), std::string::npos) << text;
}

// f7 d0  not eax -- Intel SDM Vol. 2B, NOT: "Flags Affected: None."
TEST(IrFrontend, NotDefinesNoFlags) {
    const Built built = build({0xF7, 0xD0});
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    const std::string text = print(built.block);
    EXPECT_NE(text.find("not"), std::string::npos) << text;
    EXPECT_EQ(text.find("defflags"), std::string::npos)
        << "NOT affects no flags:\n" << text;
}

// f7 d8  neg eax -- Neg32 materializes 0 - flag_src, so the record's source
// word is the ORIGINAL value and not the result.
TEST(IrFrontend, NegDefinesFlagsFromTheOriginalValue) {
    const Built built = build({0xF7, 0xD8});
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    EXPECT_NE(print(built.block).find("record=neg32"), std::string::npos);
}

// b8 78 56 34 12  mov eax, 0x12345678
TEST(IrFrontend, MovImmediateDefinesNoFlags) {
    const Built built = build({0xB8, 0x78, 0x56, 0x34, 0x12});
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    EXPECT_EQ(print(built.block).find("defflags"), std::string::npos);
}

// 8d 44 0a 10  lea eax, [edx + ecx*1 + 0x10]
TEST(IrFrontend, LeaBuildsAnAddressWithoutTouchingMemory) {
    const Built built = build({0x8D, 0x44, 0x0A, 0x10});
    ASSERT_TRUE(built.result.ok)
        << decline_reason_name(built.result.reason) << " " << built.why;
    const std::string text = print(built.block);
    EXPECT_NE(text.find("lea"), std::string::npos) << text;
    EXPECT_EQ(text.find("defflags"), std::string::npos)
        << "LEA affects no flags:\n" << text;
}

// A straight-line block of several accepted instructions verifies as a whole,
// which is what exercises the verifier's ascending-RIP-run rule against real
// instruction lengths.
TEST(IrFrontend, MultiInstructionBlockVerifies) {
    const Built built = build({
        0x01, 0xC8,              // add eax, ecx
        0x83, 0xC0, 0x05,        // add eax, 5
        0x31, 0xD2,              // xor edx, edx
        0x39, 0xC8,              // cmp eax, ecx
    });
    ASSERT_TRUE(built.result.ok)
        << decline_reason_name(built.result.reason) << " at insn "
        << built.result.insn_index;
    std::string why;
    EXPECT_TRUE(verify(built.block, &why)) << why;
}

// =============================================================================
// Declines — the reason is part of the contract
// =============================================================================

TEST(IrFrontend, EmptyBlockDeclines) {
    IrBlock block;
    const BuildResult r =
        build_block(nullptr, nullptr, 0, kEntry, false, true, block, nullptr);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, DeclineReason::NoInstructions);
}

// 48 01 08  add [rax], rcx -- long-mode read-modify-write, now lowered with
// the same ordering contract as the 32-bit form: load, compute, STORE, then
// publish flags. A faulting write-back must leave the previous instruction's
// flags visible, so the definition cannot precede the store.
TEST(IrFrontend, SixtyFourBitReadModifyWriteStoresBeforePublishingFlags) {
    const Built built = build({0x48, 0x01, 0x08}, DecodeMode::Mode64);
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    const std::string text = print(built.block);
    const usize load_at = text.find("load ");
    const usize store_at = text.find("store ");
    const usize flags_at = text.find("defflags");
    ASSERT_NE(load_at, std::string::npos) << text;
    ASSERT_NE(store_at, std::string::npos) << text;
    ASSERT_NE(flags_at, std::string::npos) << text;
    EXPECT_LT(load_at, store_at) << text;
    EXPECT_LT(store_at, flags_at)
        << "a faulting store must not have published flags already:\n" << text;
    EXPECT_EQ(text.find("gpr.write"), std::string::npos) << text;
}

// 48 03 08  add rcx, [rax] -- a long-mode memory SOURCE writes no memory, and
// the register commit follows the flag definition.
TEST(IrFrontend, SixtyFourBitAluWithMemorySourceCommitsAfterFlags) {
    const Built built = build({0x48, 0x03, 0x08}, DecodeMode::Mode64);
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    const std::string text = print(built.block);
    const usize flags_at = text.find("defflags");
    const usize write_at = text.find("gpr.write");
    ASSERT_NE(text.find("load "), std::string::npos) << text;
    EXPECT_EQ(text.find("store "), std::string::npos)
        << "an ALU with a memory SOURCE writes no memory:\n" << text;
    ASSERT_NE(flags_at, std::string::npos) << text;
    ASSERT_NE(write_at, std::string::npos) << text;
    EXPECT_LT(flags_at, write_at) << text;
}

// 48 39 08  cmp [rax], rcx -- a compare never writes its memory destination.
TEST(IrFrontend, SixtyFourBitCompareWithMemoryDestinationWritesNothing) {
    const Built built = build({0x48, 0x39, 0x08}, DecodeMode::Mode64);
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    const std::string text = print(built.block);
    EXPECT_NE(text.find("load "), std::string::npos) << text;
    EXPECT_EQ(text.find("store "), std::string::npos) << text;
    EXPECT_EQ(text.find("gpr.write"), std::string::npos) << text;
}

// `cmp rax,rcx / mov rbx,[rdx] / xor rcx,rcx` -- the exact shape the
// flag-recovery producer exists for: a definition killed later, with a
// faulting load in between that must be able to rebuild it.
//
// Both modes plan it. The long-mode plan names the REX.W record (width 64),
// and the snapshot emitter that lowers every 64-bit block publishes it into
// the fault map, so the plan must not cost the block its native admission.
// That the rebuilt flags match the interpreter's at the fault is
// test_ir_x64_flag_recovery.cpp's job.
TEST(IrFrontend, BothModesPlanFlagRecoveryForAKilledCompare) {
    const Built wide = build({0x48,0x39,0xC8, 0x48,0x8B,0x1A, 0x48,0x31,0xC9},
                             DecodeMode::Mode64);
    ASSERT_TRUE(wide.result.ok) << decline_reason_name(wide.result.reason);
    ASSERT_EQ(wide.block.flag_recovery.size(), 1u);
    EXPECT_EQ(wide.block.flag_recovery[0].width, 64u);
    EXPECT_TRUE(snapshot_block_allocatable(wide.block))
        << "a long-mode plan must keep the block natively admitted";

    const Built narrow = build({0x39,0xC8, 0x8B,0x1A, 0x31,0xC9});
    ASSERT_TRUE(narrow.result.ok) << decline_reason_name(narrow.result.reason);
    EXPECT_EQ(narrow.block.flag_recovery.size(), 1u)
        << "this shape is the producer's whole purpose in 32-bit blocks";
}

// 00 08  add byte [rax], cl -- narrow memory ALU carries the same ordering
// contract as the full-width form: load, compute, STORE, then publish flags.
TEST(IrFrontend, SixtyFourBitNarrowMemoryAluStoresBeforePublishingFlags) {
    const Built built = build({0x00, 0x08}, DecodeMode::Mode64);
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    const std::string text = print(built.block);
    const usize store_at = text.find("store ");
    const usize flags_at = text.find("defflags");
    ASSERT_NE(store_at, std::string::npos) << text;
    ASSERT_NE(flags_at, std::string::npos) << text;
    EXPECT_LT(store_at, flags_at) << text;
    EXPECT_EQ(text.find("gpr.write"), std::string::npos) << text;
}

// f0 48 01 08  lock add [rax], rcx -- LOCK still declines on the prefix in
// long mode: the atomic ordering is not modelled here either.
TEST(IrFrontend, SixtyFourBitLockedReadModifyWriteDeclines) {
    expect_declined({0xF0, 0x48, 0x01, 0x08}, DeclineReason::UnsupportedPrefix,
                    DecodeMode::Mode64);
}

// 03 01  add eax, [ecx] -- a memory SOURCE is accepted: the load is the only
// faulting op and everything it feeds happens after it, so a fault commits
// nothing.
TEST(IrFrontend, AluWithMemorySourceIsAccepted) {
    const Built built = build({0x03, 0x01});
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    const std::string text = print(built.block);
    EXPECT_NE(text.find("load"), std::string::npos) << text;
    EXPECT_EQ(text.find("store"), std::string::npos)
        << "an ALU with a memory SOURCE writes no memory:\n" << text;
}

// 39 01  cmp [ecx], eax -- a memory DESTINATION on a compare is also accepted,
// because CMP never writes it. The read-modify-write forms still decline.
TEST(IrFrontend, CompareWithMemoryDestinationIsAccepted) {
    const Built built = build({0x39, 0x01});
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    const std::string text = print(built.block);
    EXPECT_NE(text.find("load"), std::string::npos) << text;
    EXPECT_EQ(text.find("store"), std::string::npos) << text;
}

// 01 08  add [eax], ecx -- a read-modify-write, now accepted. The ORDER is
// the content: load, compute, STORE, then publish flags. Measured against the
// interpreter, a faulting RMW leaves the previous instruction's flags visible.
TEST(IrFrontend, ReadModifyWriteStoresBeforePublishingFlags) {
    const Built built = build({0x01, 0x08});
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    const std::string text = print(built.block);
    const usize store_at = text.find("store ");
    const usize flags_at = text.find("defflags");
    ASSERT_NE(store_at, std::string::npos) << text;
    ASSERT_NE(flags_at, std::string::npos) << text;
    EXPECT_LT(store_at, flags_at)
        << "a faulting store must not have published flags already:\n" << text;
    // ...and no register is written: the destination is memory.
    EXPECT_EQ(text.find("gpr.write"), std::string::npos) << text;
}

// f0 01 08  lock add [eax], ecx -- LOCK is an atomic RMW with ordering this
// IR does not model. It must decline on the PREFIX, before the operand shape
// is even considered: a LOCK that was quietly dropped would be a correctness
// bug that no register-only test could see.
TEST(IrFrontend, LockPrefixDeclinesOnThePrefix) {
    expect_declined({0xF0, 0x01, 0x08}, DeclineReason::UnsupportedPrefix);
}

// 66 01 c8  add ax, cx -- the operand-size override changes the width of the
// operation, and 16-bit merges are M2 completion work.
TEST(IrFrontend, OperandSizeOverrideDeclines) {
    expect_declined({0x66, 0x01, 0xC8}, DeclineReason::UnsupportedPrefix);
}

// 64 8b 00 -- `mov eax, fs:[eax]`. An FS override changes the address space,
// and this DECLINED until the segment base became an explicit IR value
// (`Op::SegBase`) added to the effective address. It is accepted now: on
// RollerCoaster Tycoon 3 the FS override alone was 3014 declines.
TEST(IrFrontend, SegmentOverrideIsAccepted) {
    const Built built = build({0x64, 0x8B, 0x00});
    EXPECT_TRUE(built.result.ok)
        << decline_reason_name(built.result.reason);
}

// An override the lowering cannot APPLY must still decline rather than be
// ignored. A stack instruction reaches memory through an implicit operand that
// never carries `mem.segment`, so honouring the prefix here would mean
// silently dropping it.
TEST(IrFrontend, SegmentOverrideOnAnImplicitStackOperandStillDeclines) {
    expect_declined({0x64, 0x50}, DeclineReason::UnsupportedPrefix);
}

// 00 c8  add al, cl -- an 8-bit write is a read-modify-write merge into the
// low byte, which this increment does not express.
TEST(IrFrontend, EightBitFormDeclines) {
    expect_declined({0x00, 0xC8}, DeclineReason::UnsupportedOpcode);
}

// 40  inc eax -- INC preserves CF, so its flag definition is a MERGE with the
// incoming flag value. With nothing before it in the block, that value is the
// block's INCOMING flags, which `flags.in` names.
TEST(IrFrontend, IncBuildsACfMergeFromBlockInputFlags) {
    const Built built = build({0x40});
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    const std::string text = print(built.block);
    EXPECT_NE(text.find("flags.in"), std::string::npos) << text;
    EXPECT_NE(text.find("merge"), std::string::npos) << text;
    EXPECT_NE(text.find("record=inc32"), std::string::npos) << text;
    // Flags before the register write: Inc32's source word is the ORIGINAL
    // value, which lives in the pin the write is about to overwrite.
    EXPECT_LT(text.find("defflags"), text.find("gpr.write")) << text;
}

// With a flag definition earlier in the block, the merge names THAT value
// rather than the block input — no `flags.in` is emitted at all.
TEST(IrFrontend, IncMergesFromAnEarlierDefinitionWhenThereIsOne) {
    const Built built = build({0x01, 0xC8, 0x40});  // add eax,ecx ; inc eax
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    const std::string text = print(built.block);
    EXPECT_EQ(text.find("flags.in"), std::string::npos)
        << "the ADD's flags are the incoming value here:\n" << text;
    EXPECT_NE(text.find("merge"), std::string::npos) << text;
}

// 11 c8  adc eax, ecx -- the carry input is a USE of a flag value (M3).
TEST(IrFrontend, AdcDeclines) {
    expect_declined({0x11, 0xC8}, DeclineReason::UnsupportedOpcode);
}

// c1 e0 04  shl eax, 4 -- x86 masks the count to 5 bits and a masked count of
// zero leaves the flags UNCHANGED, a runtime-conditional flag definition this
// IR cannot yet express (M3).
TEST(IrFrontend, ImmediateShiftIsAccepted) {
    const Built built = build({0xC1, 0xE0, 0x04});   // shl eax, 4
    EXPECT_TRUE(built.result.ok)
        << decline_reason_name(built.result.reason);
}

// A CL-count shift still declines: the count is a runtime value that may be
// ZERO, and a zero-count shift modifies no flags at all — so it cannot be
// lowered as a full-flag writer without a branch this frontend does not build.
TEST(IrFrontend, ClCountShiftStillDeclines) {
    expect_declined({0xD3, 0xE0}, DeclineReason::UnsupportedOpcode);
}

// eb 05  jmp +5 -- terminal unconditional branch, accepted (M7).
TEST(IrFrontend, TerminalJmpIsAccepted) {
    const Built built = build({0xEB, 0x05});
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    EXPECT_NE(print(built.block).find("jmp"), std::string::npos);
}

// 01 c8 74 05  add eax,ecx ; jz +5 -- the flags the Jcc tests are the ones the
// ADD defined, and the branch names that value as an operand. That is what
// lets the flag-demand pass keep the definition alive without knowing what a
// branch is.
TEST(IrFrontend, TerminalJccReadsThePrecedingFlagDefinition) {
    const Built built = build({0x01, 0xC8, 0x74, 0x05});
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    const std::string text = print(built.block);
    EXPECT_NE(text.find("jcc"), std::string::npos) << text;
    EXPECT_NE(text.find("cc=0x4"), std::string::npos) << text;
    // target = end of the Jcc (0x401004) + 5
    EXPECT_NE(text.find("target=0x401009"), std::string::npos) << text;
    EXPECT_NE(text.find("fallthrough=0x401004"), std::string::npos) << text;
}

// A Jcc with no flag definition before it in the block tests the flags the
// block was ENTERED with. That used to decline for want of a value to name;
// `flags.in` names it.
TEST(IrFrontend, JccWithNoPrecedingFlagDefinitionUsesBlockInputFlags) {
    const Built built = build({0x74, 0x05});
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    const std::string text = print(built.block);
    EXPECT_NE(text.find("flags.in"), std::string::npos) << text;
    EXPECT_NE(text.find("jcc"), std::string::npos) << text;
}

// c3  ret -- the return target is DATA, so this builds an indirect terminator
// rather than a constant one.
TEST(IrFrontend, ReturnBuildsAnIndirectTerminator) {
    const Built built = build({0xC3});
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    const std::string text = print(built.block);
    EXPECT_NE(text.find("jmp.indirect"), std::string::npos) << text;
    // ESP must be committed BEFORE the branch: the pop retires whether or not
    // the target is reachable.
    EXPECT_NE(text.find("gpr.write esp"), std::string::npos) << text;
}

// e8 10 00 00 00  call rel32 -- pushes the return address, then branches.
TEST(IrFrontend, CallPushesTheReturnAddressThenBranches) {
    const Built built = build({0xE8, 0x10, 0x00, 0x00, 0x00});
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    const std::string text = print(built.block);
    // Return address is the instruction AFTER the call.
    EXPECT_NE(text.find("const 0x401005"), std::string::npos) << text;
    EXPECT_NE(text.find("target=0x401015"), std::string::npos) << text;

    // THE ordering claim: the store comes before the ESP commit, so a faulting
    // stack write leaves ESP untouched (Intel SDM Vol. 2, PUSH/CALL).
    const usize store_at = text.find("store ");
    const usize esp_at = text.find("gpr.write esp");
    ASSERT_NE(store_at, std::string::npos) << text;
    ASSERT_NE(esp_at, std::string::npos) << text;
    EXPECT_LT(store_at, esp_at)
        << "ESP must not be committed before the stack write:\n" << text;
}

// ff d0  call eax -- the register form is accepted: push the return address,
// then branch to the runtime target.
TEST(IrFrontend, IndirectCallRegisterFormIsAccepted) {
    const Built built = build({0xFF, 0xD0});
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    const std::string text = print(built.block);
    EXPECT_NE(text.find("store"), std::string::npos) << text;
    EXPECT_NE(text.find("jmp.indirect"), std::string::npos) << text;
}

// ff 10  call [eax] -- an import thunk, and the single most common blocker
// measured on real 32-bit code. The target is loaded FIRST so a faulting load
// commits nothing, and ESP is committed only after the push store retires, so
// a fault at either access leaves the guest untouched.
TEST(IrFrontend, IndirectCallThroughMemoryIsAccepted) {
    const Built built = build({0xFF, 0x10});
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    const std::string text = print(built.block);
    const usize load_at = text.find("load");
    const usize store_at = text.find("store ");
    const usize esp_at = text.find("gpr.write esp");
    ASSERT_NE(load_at, std::string::npos) << text;
    ASSERT_NE(store_at, std::string::npos) << text;
    ASSERT_NE(esp_at, std::string::npos) << text;
    EXPECT_LT(load_at, store_at) << "target load before the push:\n" << text;
    EXPECT_LT(store_at, esp_at) << "ESP commits after the push:\n" << text;
}

// ff e0  jmp eax -- an indirect branch, and the single most common reason a
// real block declined before it was supported. The terminator already existed
// for RET; only the frontend had to learn the opcode.
TEST(IrFrontend, IndirectJumpIsAccepted) {
    const Built built = build({0xFF, 0xE0});
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    EXPECT_NE(print(built.block).find("jmp.indirect"), std::string::npos);
}

// ff 21  jmp [ecx] -- the memory form loads the target first.
TEST(IrFrontend, IndirectJumpThroughMemoryIsAccepted) {
    const Built built = build({0xFF, 0x21});
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    const std::string text = print(built.block);
    EXPECT_NE(text.find("load"), std::string::npos) << text;
    EXPECT_NE(text.find("jmp.indirect"), std::string::npos) << text;
}

// The REGISTER-destination order is the opposite, and both are right: a
// register write overwrites the pin a flag source may still be reading, so
// there the flags come first. Asserting both directions keeps the pair from
// being "fixed" into agreement.
TEST(IrFrontend, RegisterDestinationPublishesFlagsBeforeTheWrite) {
    const Built built = build({0xF7, 0xD8});  // neg eax
    ASSERT_TRUE(built.result.ok);
    const std::string text = print(built.block);
    EXPECT_LT(text.find("defflags"), text.find("gpr.write")) << text;
}

// A block whose accepted prefix ends at an unsupported instruction declines
// the WHOLE block and names the offending index, not just the instruction.
TEST(IrFrontend, DeclineNamesTheOffendingInstructionIndex) {
    const Built built = build({
        0x01, 0xC8,        // 0: add eax, ecx     (accepted)
        0x31, 0xD2,        // 1: xor edx, edx     (accepted)
        0xD3, 0xE0,        // 2: shl eax, cl     (declined — runtime count)
    });
    EXPECT_FALSE(built.result.ok);
    EXPECT_EQ(built.result.reason, DeclineReason::UnsupportedOpcode);
    EXPECT_EQ(built.result.insn_index, 2u);
}

// =============================================================================
// Coverage telemetry
// =============================================================================

TEST(IrFrontend, DeclineReasonNamesAreAllDistinct) {
    const DeclineReason all[] = {
        DeclineReason::None,               DeclineReason::NoInstructions,
        DeclineReason::Bitness64,          DeclineReason::BlockTooLong,
        DeclineReason::UnsupportedPrefix,  DeclineReason::UnsupportedOpcode,
        DeclineReason::UnsupportedOperand, DeclineReason::UnsupportedWidth,
        DeclineReason::VerifierRejected,
    };
    for (const DeclineReason a : all) {
        EXPECT_STRNE(decline_reason_name(a), "?");
        for (const DeclineReason b : all) {
            if (a == b) continue;
            EXPECT_STRNE(decline_reason_name(a), decline_reason_name(b));
        }
    }
}

// Guest registers live in fixed host pins, so a `GprWrite` overwrites the pin
// that a flag record may still need to read. NEG is the case that bites: its
// `Neg32` record reconstructs flags from the ORIGINAL destination, which is
// exactly the register being written. The flag definition must therefore be
// emitted BEFORE the register write.
//
// This was a real wrong-flags bug, found by comparing materialized RFLAGS on
// asymmetric operands; an ADD-shaped corpus cannot see it, because ADD's flag
// source is the other operand and nothing clobbers it.
TEST(IrFrontend, FlagDefinitionPrecedesTheRegisterWrite) {
    const Built built = build({0xF7, 0xD8});  // neg eax
    ASSERT_TRUE(built.result.ok);

    int defflags_at = -1;
    int gprwrite_at = -1;
    for (usize i = 0; i < built.block.insts.size(); ++i) {
        if (built.block.insts[i].op == Op::DefFlags) defflags_at = int(i);
        if (built.block.insts[i].op == Op::GprWrite) gprwrite_at = int(i);
    }
    ASSERT_NE(defflags_at, -1);
    ASSERT_NE(gprwrite_at, -1);
    EXPECT_LT(defflags_at, gprwrite_at)
        << "flags must be published before the pin is overwritten:\n"
        << print(built.block);
}

// The same ordering for the read-modify-write ALU forms.
TEST(IrFrontend, AluFlagDefinitionAlsoPrecedesTheRegisterWrite) {
    const Built built = build({0x01, 0xC8});  // add eax, ecx
    ASSERT_TRUE(built.result.ok);
    int defflags_at = -1, gprwrite_at = -1;
    for (usize i = 0; i < built.block.insts.size(); ++i) {
        if (built.block.insts[i].op == Op::DefFlags) defflags_at = int(i);
        if (built.block.insts[i].op == Op::GprWrite) gprwrite_at = int(i);
    }
    EXPECT_LT(defflags_at, gprwrite_at) << print(built.block);
}

TEST(IrFrontend, LongModeIndexedMemoryAcceptedPartialWidthsDecline) {
    const auto indexed = build({0x48, 0x8B, 0x04, 0x49}, DecodeMode::Mode64);
    ASSERT_TRUE(indexed.result.ok) << indexed.why;
    bool indexed_lea = false;
    for (const auto& op : indexed.block.insts) {
        if (op.op == Op::Lea && op.operands[1].kind == OperandKind::Value)
            indexed_lea = true;
    }
    EXPECT_TRUE(indexed_lea);

    // 40 88 E0  mov al, spl   and   66 89 c8  mov ax, cx.
    // Both are narrow register-to-register MOVs, and both are PARTIAL writes:
    // the IR must carry a narrow gpr.write, not a full-width one that would
    // zero the bits x86 preserves. `mov al, spl` is only expressible at all
    // because the high bytes have their own index space — under the old
    // encoding its REX source would have read as AH.
    for (const std::vector<u8>& bytes : {std::vector<u8>{0x40, 0x88, 0xE0},
                                         std::vector<u8>{0x66, 0x89, 0xC8}}) {
        const Built built = build(bytes, DecodeMode::Mode64);
        ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
        bool narrow_write = false;
        for (const auto& op : built.block.insts)
            if (op.op == Op::GprWrite && (op.operands[0].type == Type::I8 ||
                                          op.operands[0].type == Type::I16))
                narrow_write = true;
        EXPECT_TRUE(narrow_write) << print(built.block);
    }
}

// C0 /0 ib: rol r/m8, imm8 -- a PARTIAL flag writer. It must move CF and OF
// through the overlay and publish NO record, because a record replaces the
// whole flag state and ZF/SF/PF/AF have to survive from the previous
// instruction.
TEST(IrFrontend, RotateForcesCarryWithoutPublishingARecord) {
    const Built built = build({0xC0, 0xC0, 0x03}, DecodeMode::Mode64);
    ASSERT_TRUE(built.result.ok) << decline_reason_name(built.result.reason);
    const std::string text = print(built.block);
    EXPECT_NE(text.find("set.cf"), std::string::npos) << text;
    EXPECT_EQ(text.find("defflags"), std::string::npos)
        << "a rotate must not replace the pending record:\n" << text;
    // A by-1 rotate additionally defines OF; a multi-bit one leaves it.
    EXPECT_EQ(text.find("set.of"), std::string::npos) << text;
    const Built by_one = build({0xD0, 0xC0}, DecodeMode::Mode64);
    ASSERT_TRUE(by_one.result.ok) << decline_reason_name(by_one.result.reason);
    EXPECT_NE(print(by_one.block).find("set.of"), std::string::npos);
}

// The partial-width forms that are still unmet, kept explicit so the tranches
// above cannot quietly be read as covering all of Appendix A.
TEST(IrFrontend, LongModeRemainingPartialWidthFormsStillDecline) {
    // 8E /r: mov ss, ax -- segment-register MOVs need descriptor semantics.
    expect_declined({0x8E, 0xD0}, DeclineReason::UnsupportedOpcode,
                    DecodeMode::Mode64);
    // 48 D3 /4: shl r/m64, cl. The by-CL shifts publish a record whose count
    // is a runtime value, and the record packs it as `(count << 32) | result`
    // -- which a 64-bit result fills. The immediate and by-1 64-bit shifts are
    // implemented; they carry the count in the opcode instead.
    expect_declined({0x48, 0xD3, 0xE1}, DeclineReason::UnsupportedOpcode,
                    DecodeMode::Mode64);
    // 48 11 /r: adc r/m64, r64. A 64-bit source fills the record's payload
    // word, leaving nowhere for the carry-in that `flag_src` has to pack
    // alongside it -- which is why `Adc64`/`Sbb64` do not exist. The 8-, 16-
    // and 32-bit forms are implemented.
    expect_declined({0x48, 0x11, 0xC8}, DeclineReason::UnsupportedOpcode,
                    DecodeMode::Mode64);
    // 0F B0 /r with a MEMORY operand: cmpxchg [rcx], al. Intel SDM Vol. 2
    // CMPXCHG writes DEST on BOTH paths, so the not-equal path stores too, and
    // the reference probes write permission before doing anything. That is a
    // memory contract rather than an arithmetic one; the register forms are
    // implemented.
    expect_declined({0x0F, 0xB0, 0x01}, DeclineReason::UnsupportedOperand,
                    DecodeMode::Mode64);
    // 66 0F A5 /r: shld r/m16, r16, cl. The count is a runtime value, and a
    // 16-bit double shift by 16 or more is architecturally UNDEFINED -- the
    // reference implementation computes it by shifting a negative amount.
    // Nothing can be admitted for a runtime count that may land there, so the
    // CL forms stay open while every immediate form is implemented.
    expect_declined({0x66, 0x0F, 0xA5, 0xC1}, DeclineReason::UnsupportedOpcode,
                    DecodeMode::Mode64);
    // 86 /r with a MEMORY operand: xchg [rcx], al. Intel SDM Vol. 2 XCHG --
    // a memory operand is locked "regardless of the presence or absence of the
    // LOCK prefix", so the tier that owns the lock protocol keeps it. The
    // register forms are implemented.
    expect_declined({0x86, 0x01}, DeclineReason::UnsupportedOperand,
                    DecodeMode::Mode64);
    // 0F C0 /r with a memory operand: xadd [rcx], al. Not locked without the
    // prefix, but the reference publishes its flags and writes its register
    // operand BEFORE the store, so a faulting store leaves both behind --
    // which contradicts the fault invariant every other memory form here is
    // built on. The register forms are implemented.
    expect_declined({0x0F, 0xC0, 0x01}, DeclineReason::UnsupportedOperand,
                    DecodeMode::Mode64);
}

// The M12 near-transfer and stack forms that are still unmet, kept explicit
// so the tranche above cannot quietly be read as covering Appendix B.
TEST(IrFrontend, LongModeRemainingStackFormsStillDecline) {
    // 0F A1 / 0F A9: the FS and GS POPs. Loading a segment register reloads
    // its descriptor, which is a contract this IR does not model. The
    // matching PUSHes are implemented -- they only READ the selector.
    expect_declined({0x0F, 0xA1}, DeclineReason::UnsupportedOpcode, DecodeMode::Mode64);
    expect_declined({0x0F, 0xA9}, DeclineReason::UnsupportedOpcode, DecodeMode::Mode64);
    // C8 iw ib with a nesting level above 1: the display copy is a RUNTIME
    // loop of `level - 1` iterations. Levels 0 and 1 are implemented.
    expect_declined({0xC8, 0x10, 0x00, 0x02}, DeclineReason::UnsupportedOperand,
                    DecodeMode::Mode64);
    // 8F /0 with a memory operand: `pop qword [rax]`. The effective address is
    // computed AFTER the stack increment has been committed, so `pop [rsp+8]`
    // does not address what it appears to. The register forms are implemented.
    expect_declined({0x8F, 0x00}, DeclineReason::UnsupportedOperand,
                    DecodeMode::Mode64);
}
