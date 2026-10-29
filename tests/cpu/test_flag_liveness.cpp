// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Within-block full-flag liveness (omit dead emit_set_lazy_flags stores).

#include <gtest/gtest.h>

#include <array>
#include <cstring>

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/jit/flag_liveness.hpp"
#include "core/cpu/state/flag_state.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/interpreter/interpreter.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

BasicBlock decode_bytes(const u8* code, usize len, bool is_64bit) {
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
    block.instruction_count = static_cast<u32>(block.instructions.size());
    return block;
}

// -----------------------------------------------------------------------------
// Overlay writers must never classify as flag-Neutral.
// -----------------------------------------------------------------------------
//
// This pins a coupling between two files that nothing else connects, and that
// a reader of either one would not see.
//
// CLC/STC/CMC and the BT and rotate/shift families install a CF or OF overlay
// while leaving a pending lazy FlagOp live -- that is the point of the overlay.
// Separately, compile_instruction() drops the static lazy-kind prediction for
// any instruction that classify_flag_effect() calls non-Neutral. Those two
// facts together are what stops the JIT's *predicted* condition walk from ever
// being reached with a live overlay: the writer that installed the overlay has
// already invalidated the prediction, so the consumer takes the generic walk,
// whose overlay gate is the one with test coverage
// (JitParityTest.*UsesTheOverlay).
//
// Reclassify any of these as Neutral and that stops being true. The predicted
// walk does carry its own overlay gate, so the immediate result would be
// correct -- but it would be correct on a path with no coverage, which is how
// this defect class shipped the first two times.
//
// So: this is not a test of classification for its own sake. It is the reason
// the predicted walk's gate is currently unreachable, made checkable.
//
// The classification is already doubly fail-safe, which is worth knowing
// because it says what this test can and cannot catch. classify() tests its
// Barrier set BEFORE its Neutral allow-list, and anything matching neither
// falls through to Barrier. Planting confirmed both halves: deleting CLC from
// the Barrier set leaves it Barrier by default, and adding it to the Neutral
// list leaves it Barrier because the earlier check wins. The test fires only
// on the realistic defect -- an opcode MOVED from one set to the other -- and
// that is the case it exists for.

TEST(FlagLivenessTest, OverlayWritersAreNotFlagNeutral) {
    struct Case { const char* name; std::vector<u8> bytes; };
    // One encoding per lowering that stores to cf_overlay / of_overlay.
    const std::vector<Case> writers = {
        {"CLC",          {0xF8}},
        {"STC",          {0xF9}},
        {"CMC",          {0xF5}},
        {"BT r/m32,r32", {0x0F, 0xA3, 0xD8}},
        {"BTS",          {0x0F, 0xAB, 0xD8}},
        {"BTR",          {0x0F, 0xB3, 0xD8}},
        {"BTC",          {0x0F, 0xBB, 0xD8}},
        {"ROR r/m32,1",  {0xD1, 0xC8}},
        {"ROL r/m32,1",  {0xD1, 0xC0}},
        {"ROR r/m32,CL", {0xD3, 0xC8}},
        {"RCL r/m32,1",  {0xD1, 0xD0}},
        {"RCR r/m32,1",  {0xD1, 0xD8}},
        {"SHL r/m32,imm",{0xC1, 0xE0, 0x05}},
        {"SHR r/m32,CL", {0xD3, 0xE8}},
        {"SAR r/m32,imm",{0xC1, 0xF8, 0x03}},
    };
    for (const auto& c : writers) {
        BasicBlock b = decode_bytes(c.bytes.data(), c.bytes.size(), false);
        ASSERT_EQ(b.instruction_count, 1u) << c.name << " failed to decode";
        EXPECT_NE(classify_flag_effect(b.instructions[0]), FlagEffect::Neutral)
            << c.name << " installs a CF/OF overlay over a live lazy FlagOp."
               " Classifying it Neutral leaves the lazy-kind prediction alive"
               " across it, which routes the next Jcc/SETcc/CMOVcc to the"
               " PREDICTED condition walk with an overlay live -- a path with"
               " no test coverage. See the comment above this test.";
    }
}

TEST(FlagLivenessTest, SecondAddKillsFirstBeforeJcc) {
    const u8 code[] = {0x01, 0xC8, 0x01, 0xD0, 0x74, 0x00};
    BasicBlock b = decode_bytes(code, sizeof(code), false);
    ASSERT_EQ(b.instruction_count, 3u);
    std::vector<u8> omit;
    compute_omit_lazy_flags(b, omit);
    ASSERT_EQ(omit.size(), 3u);
    EXPECT_EQ(omit[0], 1);
    EXPECT_EQ(omit[1], 0);
    EXPECT_EQ(omit[2], 0);
}

TEST(FlagLivenessTest, AddBeforeIncIsNotOmitted) {
    const u8 code[] = {0x01, 0xC8, 0xFF, 0xC0, 0x74, 0x00};
    BasicBlock b = decode_bytes(code, sizeof(code), false);
    ASSERT_EQ(b.instruction_count, 3u);
    std::vector<u8> omit;
    compute_omit_lazy_flags(b, omit);
    EXPECT_EQ(omit[0], 0);
    EXPECT_EQ(omit[1], 0);
}

TEST(FlagLivenessTest, TrailingAddIsLiveOut) {
    const u8 code[] = {0x01, 0xC8, 0xC3};
    BasicBlock b = decode_bytes(code, sizeof(code), false);
    ASSERT_EQ(b.instruction_count, 2u);
    std::vector<u8> omit;
    compute_omit_lazy_flags(b, omit);
    EXPECT_EQ(omit[0], 0);
}

TEST(FlagLivenessTest, MovBetweenAddsStillKills) {
    const u8 code[] = {
        0x01, 0xC8, 0x89, 0xC3, 0x01, 0xD0, 0x74, 0x00,
    };
    BasicBlock b = decode_bytes(code, sizeof(code), false);
    ASSERT_EQ(b.instruction_count, 4u);
    std::vector<u8> omit;
    compute_omit_lazy_flags(b, omit);
    EXPECT_EQ(omit[0], 1);
    EXPECT_EQ(omit[2], 0);
}

TEST(FlagLivenessTest, MemoryMovBetweenWritersOmitsWithRecovery) {
    // CMP EAX, 0; MOV [EBX], EAX; ADD EAX, 1. The store may fault, but CMP's
    // operands stay in EAX / imm0 so a JitFlagRecovery plan can rebuild the
    // elided record. Omitting across Neutral memory is sound once that plan
    // is published (ADR-0004 / stencil M5).
    const u8 code[] = {
        0x83, 0xF8, 0x00,
        0x89, 0x03,
        0x83, 0xC0, 0x01,
    };
    BasicBlock b = decode_bytes(code, sizeof(code), false);
    ASSERT_EQ(b.instruction_count, 3u);
    std::vector<u8> omit;
    std::vector<StencilFlagRecovery> recovery;
    compute_omit_lazy_flags(b, omit, &recovery);
    ASSERT_EQ(omit.size(), 3u);
    EXPECT_EQ(omit[0], 1);
    ASSERT_EQ(recovery.size(), 1u);
    EXPECT_TRUE(recovery[0].active());
    EXPECT_TRUE(recovery[0].recompute_dst);
    EXPECT_EQ(recovery[0].flag_op,
              static_cast<u8>(FlagState::FlagOp::Sub32));
}


TEST(FlagLivenessTest, RegisterAddAcrossLoadOmitsWithRecoveryPlan) {
    // ADD EAX, ECX; MOV EDX, [EBX]; XOR EAX, EAX — DirectDst recovery.
    const u8 code[] = {
        0x01, 0xC8,        // add eax, ecx
        0x8B, 0x13,        // mov edx, [ebx]
        0x31, 0xC0,        // xor eax, eax
    };
    BasicBlock b = decode_bytes(code, sizeof(code), false);
    ASSERT_EQ(b.instruction_count, 3u);
    std::vector<u8> omit;
    std::vector<StencilFlagRecovery> recovery;
    compute_omit_lazy_flags(b, omit, &recovery);
    EXPECT_EQ(omit[0], 1);
    ASSERT_EQ(recovery.size(), 1u);
    EXPECT_FALSE(recovery[0].recompute_dst);
    EXPECT_EQ(recovery[0].flag_op,
              static_cast<u8>(FlagState::FlagOp::Add32));
    EXPECT_FALSE(recovery[0].src_is_imm);
    EXPECT_EQ(recovery[0].src_reg, 1u);  // ECX
    EXPECT_EQ(recovery[0].dst_reg, 0u);  // EAX holds result
}

TEST(FlagLivenessTest, X64EscapingJccDefersOnlyAnAdjacentPublisher) {
    // ADD RAX,RCX; JZ outside; ADD RAX,RDX. The escaping edge still needs
    // the first ADD's flags. Adjacent to the JZ its operands are intact, so
    // the store may move onto the taken edge (emit_set_lazy_flags64 defers
    // it; JitParity64Test.MidBlockTakenJccKeepsAfPfOfOmittedLazyStore guards
    // the replay).
    const u8 adjacent[] = {0x48, 0x01, 0xC8, 0x74, 0x20, 0x48, 0x01, 0xD0};
    BasicBlock b = decode_bytes(adjacent, sizeof(adjacent), true);
    ASSERT_EQ(b.instruction_count, 3u);
    std::vector<u8> omit;
    compute_omit_lazy_flags(b, omit);
    ASSERT_EQ(omit.size(), 3u);
    EXPECT_EQ(omit[0], 1);
    EXPECT_EQ(omit[2], 0);  // live-out

    // ADD RAX,RCX; MOV R8,R9; JZ outside; ADD RAX,RDX. With an instruction
    // between them the replay's operands are not known: the store stays.
    const u8 separated[] = {0x48, 0x01, 0xC8, 0x4D, 0x89, 0xC8, 0x74, 0x20,
                            0x48, 0x01, 0xD0};
    b = decode_bytes(separated, sizeof(separated), true);
    ASSERT_EQ(b.instruction_count, 4u);
    compute_omit_lazy_flags(b, omit);
    ASSERT_EQ(omit.size(), 4u);
    EXPECT_EQ(omit[0], 0);
}

TEST(FlagLivenessTest, ShrBy1KillsPriorAddBeforeJcc) {
    // ADD eax, ecx; SHR eax, 1; JZ +0
    const u8 code[] = {0x01, 0xC8, 0xD1, 0xE8, 0x74, 0x00};
    BasicBlock b = decode_bytes(code, sizeof(code), false);
    ASSERT_EQ(b.instruction_count, 3u);
    std::vector<u8> omit;
    compute_omit_lazy_flags(b, omit);
    EXPECT_EQ(omit[0], 1);  // ADD killed by SHR
    EXPECT_EQ(omit[1], 0);  // SHR live for JZ
    EXPECT_EQ(omit[2], 0);
}

TEST(FlagLivenessTest, ShrImm0DoesNotKillPriorAdd) {
    // ADD eax, ecx; SHR eax, 0; JZ +0 — imm0 leaves flags unchanged.
    const u8 code[] = {0x01, 0xC8, 0xC1, 0xE8, 0x00, 0x74, 0x00};
    BasicBlock b = decode_bytes(code, sizeof(code), false);
    ASSERT_EQ(b.instruction_count, 3u);
    std::vector<u8> omit;
    compute_omit_lazy_flags(b, omit);
    EXPECT_EQ(omit[0], 0);  // ADD still live
    EXPECT_EQ(omit[1], 0);
}

TEST(FlagLivenessTest, ShrClIsBarrierForPriorAdd) {
    // ADD eax, edx; SHR eax, cl; JZ +0 — CL may be 0 at runtime.
    const u8 code[] = {0x01, 0xD0, 0xD3, 0xE8, 0x74, 0x00};
    BasicBlock b = decode_bytes(code, sizeof(code), false);
    ASSERT_EQ(b.instruction_count, 3u);
    std::vector<u8> omit;
    compute_omit_lazy_flags(b, omit);
    EXPECT_EQ(omit[0], 0);  // ADD must stay — SHR CL is a barrier
    EXPECT_EQ(omit[1], 0);
}

TEST(FlagLivenessTest, TestBeforeLaterCmpIsOmittedAcrossFusedJcc) {
    // TEST ebp, ebp; JGE; MOV eax, ecx; CMP eax, ebx; JZ
    // JGE fuses from TEST NZCV, so TEST's FlagOp is killed by CMP.
    const u8 code[] = {
        0x85, 0xED, 0x7D, 0x02, 0x89, 0xC8, 0x39, 0xD8, 0x74, 0x00,
    };
    BasicBlock b = decode_bytes(code, sizeof(code), false);
    ASSERT_EQ(b.instruction_count, 5u);
    std::vector<u8> omit;
    compute_omit_lazy_flags(b, omit);
    EXPECT_EQ(omit[0], 1);  // TEST killed by later CMP: JGE +2 rejoins at it
    EXPECT_EQ(omit[3], 0);  // CMP live-out
}

TEST(FlagLivenessTest, TestBeforeJbKeepsFlagOp) {
    // TEST eax, eax; JB — unsigned does not fuse from And; reconstruct
    // needs FlagOp.
    const u8 code[] = {0x85, 0xC0, 0x72, 0x00};
    BasicBlock b = decode_bytes(code, sizeof(code), false);
    ASSERT_EQ(b.instruction_count, 2u);
    std::vector<u8> omit;
    compute_omit_lazy_flags(b, omit);
    EXPECT_EQ(omit[0], 0);
}

TEST(FlagLivenessTest, TestCmpJccWalkOmitsFusedOnlyWriters) {
    // Generic TEST/CMP/Jcc walk in one translation (forward Jccs stay in
    // the block). Fused Jccs do not consume FlagOp, so mid-block TEST/CMP
    // and the mem-SUB killed by SHR/AND/CMP omit. Last CMP before JNE is
    // live-out to the dispatcher / next block.
    //
    // TEST ebp,ebp; JGE; MOV edx,[esp+18]; MOV ecx,[edx];
    // CMP ecx,[esp+20]; JB; CMP ecx,[esp+1C]; JAE;
    // SUB [esp+14],4; MOV eax,ecx; SHR eax,0xC; MOV edx,eax;
    // AND edx,7; CMP [esp+edx*8+28],eax; LEA edx,[esp+edx*8+28];
    // MOV ebx,ecx; JNE; MOV edi,[edx+4]
    const u8 code[] = {
        0x85, 0xED, 0x0F, 0x8D, 0x2F, 0x01, 0x00, 0x00, 0x8B, 0x54,
        0x24, 0x18, 0x8B, 0x0A, 0x3B, 0x4C, 0x24, 0x20, 0x0F, 0x82,
        0x1F, 0x01, 0x00, 0x00, 0x3B, 0x4C, 0x24, 0x1C, 0x0F, 0x83,
        0x15, 0x01, 0x00, 0x00, 0x83, 0x6C, 0x24, 0x14, 0x04, 0x8B,
        0xC1, 0xC1, 0xE8, 0x0C, 0x8B, 0xD0, 0x83, 0xE2, 0x07, 0x39,
        0x44, 0xD4, 0x28, 0x8D, 0x54, 0xD4, 0x28, 0x8B, 0xD9, 0x75,
        0x05, 0x8B, 0x7A, 0x04,
    };
    BasicBlock b = decode_bytes(code, sizeof(code), false);
    ASSERT_EQ(b.instruction_count, 18u);
    std::vector<u8> omit;
    compute_omit_lazy_flags(b, omit);
    ASSERT_EQ(omit.size(), 18u);
    // Escaping Jcc cold stubs still re-emit on the taken edge. Fall-through
    // Neutral loads are covered by stencil M5 recovery plans. A FullWriter
    // with memory is a killer only under such a plan too: its own access can
    // fault before it writes a flag, and the exception context must then hold
    // the predecessor's flags (Intel SDM Vol. 3A 6.6). A memory-operand CMP
    // publisher has no plan -- its loaded operand is gone -- so it keeps its
    // record when the killer is a memory access.
    EXPECT_EQ(omit[0], 1);   // TEST — recovered across loads to later CMP
    EXPECT_EQ(omit[4], 0);   // CMP [mem] — next killer CMP [mem] may fault
    EXPECT_EQ(omit[6], 0);   // CMP [mem] — killer SUB [mem] may fault
    EXPECT_EQ(omit[8], 1);   // SUB [mem] — killed by SHR (and AND/CMP)
    EXPECT_EQ(omit[10], 1);  // SHR imm — killed by AND
    EXPECT_EQ(omit[12], 1);  // AND — recovered across following mem CMP
    EXPECT_EQ(omit[13], 0);  // last CMP — live-out (fused JNE is not a kill)
}

TEST(FlagLivenessTest, LoopSubKillsWalkCmpButStaysLiveOut) {
    // Same walk plus `SUB esi,1; JNZ` (counter after the body). The walk
    // CMP is no longer live-out; the loop SUB is.
    //
    // The `75 03` near the end targets the SUB at 0x1040 (index 18), so
    // ARCHITECTURALLY the killer is on both edges. That is not enough, and
    // this expectation has now been corrected a second time.
    //
    // control.cpp compiles every non-terminal, non-loop-back Jcc as
    // queue_mid_block_jcc_exit: a cold exit that publishes RIP and
    // re-dispatches. So an in-block forward target does NOT rejoin this
    // translation — the block EXITS there, with the walk CMP's flags live and
    // its lazy record omitted, and the killer runs only in the next
    // translation. The successor can then consume a stale record (or the
    // published nzcv kind) and evaluate a condition wrongly.
    //
    // That is the trace-formation bug: measured as Steam dying 16,368
    // instructions in at RIP=0x14009ea3b. The shape only occurs when a block
    // traces past a forward branch, which is why it went unseen — and note
    // this is a 32-bit block, so it is also the recorded 32-bit trace hazard.
    //
    // If in-block forward branches ever become real intra-block branches
    // instead of exits, the old expectation becomes correct again and both
    // this test and the `max_taken_target` logic should be restored together.
    const u8 code[] = {
        0x85, 0xED, 0x0F, 0x8D, 0x2F, 0x01, 0x00, 0x00, 0x8B, 0x54,
        0x24, 0x18, 0x8B, 0x0A, 0x3B, 0x4C, 0x24, 0x20, 0x0F, 0x82,
        0x1F, 0x01, 0x00, 0x00, 0x3B, 0x4C, 0x24, 0x1C, 0x0F, 0x83,
        0x15, 0x01, 0x00, 0x00, 0x83, 0x6C, 0x24, 0x14, 0x04, 0x8B,
        0xC1, 0xC1, 0xE8, 0x0C, 0x8B, 0xD0, 0x83, 0xE2, 0x07, 0x39,
        0x44, 0xD4, 0x28, 0x8D, 0x54, 0xD4, 0x28, 0x8B, 0xD9, 0x75,
        0x03, 0x8B, 0x7A, 0x04, 0x83, 0xEE, 0x01, 0x75, 0x00,
    };
    BasicBlock b = decode_bytes(code, sizeof(code), false);
    ASSERT_EQ(b.instruction_count, 20u);
    std::vector<u8> omit;
    compute_omit_lazy_flags(b, omit);
    ASSERT_EQ(omit.size(), 20u);
    EXPECT_EQ(omit[13], 0);  // walk CMP: the Jcc EXITS before the killer
    EXPECT_EQ(omit[18], 0);  // SUB esi,1 live-out before JNZ
}

TEST(FlagLivenessTest, DeadShrBeforeAndIsOmitted) {
    // SHR eax, 1; AND eax, eax; JZ +0
    const u8 code[] = {0xD1, 0xE8, 0x21, 0xC0, 0x74, 0x00};
    BasicBlock b = decode_bytes(code, sizeof(code), false);
    ASSERT_EQ(b.instruction_count, 3u);
    std::vector<u8> omit;
    compute_omit_lazy_flags(b, omit);
    EXPECT_EQ(omit[0], 1);  // SHR killed by AND
    EXPECT_EQ(omit[1], 0);  // AND live for JZ
}

TEST(FlagLivenessTest, JitMatchesInterpreterWhenFirstAddOmitted) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(0x20000, 0x1000, MemoryProtection::ReadWrite));

    const u8 code[] = {
        0x01, 0xC8,
        0x01, 0xD0,
        0x74, 0x05,
        0xB8, 0x11, 0x00, 0x00, 0x00,
        0xC3,
        0xB8, 0x22, 0x00, 0x00, 0x00,
        0xC3,
    };
    ASSERT_TRUE(memory.write_bytes(0x1000, code, sizeof(code)));

    const std::array<std::array<u32, 3>, 4> cases = {{
        {{1, 2, 3}},
        {{0, 0, 0}},
        {{5, 0xFFFFFFFBu, 0}},
        {{0x10, 0x20, 0x30}},
    }};

    for (const auto& t : cases) {
        CpuState ii;
        ii.init_protected_mode();
        ii.gpr.set32(0, t[0]);
        ii.gpr.set32(1, t[1]);
        ii.gpr.set32(2, t[2]);
        ii.gpr.set32(4, 0x20800);
        ii.rip = 0x1000;
        ASSERT_TRUE(memory.write_u32(0x20800, 0));
        {
            Interpreter interp(ii, memory);
            for (int i = 0; i < 32 && ii.rip != 0; ++i) {
                if (interp.step() != ExecResult::Continue) break;
            }
        }

        CpuState ji;
        ji.init_protected_mode();
        ji.gpr.set32(0, t[0]);
        ji.gpr.set32(1, t[1]);
        ji.gpr.set32(2, t[2]);
        ji.gpr.set32(4, 0x20800);
        ji.rip = 0x1000;
        ASSERT_TRUE(memory.write_u32(0x20800, 0));
        {
            JitCompiler jit(memory);
            for (int guard = 0; guard < 16 && ji.rip != 0; ++guard) {
                u8 local[32];
                for (int k = 0; k < 32; ++k) {
                    auto r = memory.read_u8(
                        static_cast<GuestAddr>(ji.rip + k));
                    local[k] = r.is_ok() ? r.value() : 0xCC;
                }
                BasicBlock block;
                block.start_addr = static_cast<GuestAddr>(ji.rip);
                block.is_64bit = false;
                MemoryByteReader reader(local, sizeof(local));
                while (reader.remaining() > 0) {
                    DecodedInstruction inst;
                    if (decode_instruction(reader, DecodeMode::Mode32, inst)
                        != DecodeError::Success) {
                        break;
                    }
                    block.instructions.push_back(inst);
                    const char* nm = opcode_name(inst.opcode);
                    if (nm && (std::strncmp(nm, "J", 1) == 0 ||
                               std::strncmp(nm, "RET", 3) == 0)) {
                        break;
                    }
                }
                block.byte_length = 0;
                for (const auto& in : block.instructions)
                    block.byte_length += in.length;
                block.instruction_count =
                    static_cast<u32>(block.instructions.size());
                if (block.instruction_count == 0) break;
                CompiledBlock* cb = jit.compile(block);
                if (!cb) {
                    Interpreter interp(ji, memory);
                    if (interp.step() != ExecResult::Continue) break;
                    continue;
                }
                jit.execute(*cb, ji);
            }
        }

        EXPECT_EQ(ji.gpr.get32(0), ii.gpr.get32(0))
            << "eax=" << t[0] << " ecx=" << t[1] << " edx=" << t[2];
        ji.materialize_lazy_flags();
        ii.materialize_lazy_flags();
        EXPECT_EQ(ji.flags.rflags & 0x8D5u, ii.flags.rflags & 0x8D5u)
            << "arith flags mismatch";
    }
}

TEST(FlagLivenessTest, JitMatchesInterpreterWhenDeadShrOmitted) {
    // SHR eax,1 ; AND eax,eax ; JZ → mov eax,0x11;ret / mov eax,0x22;ret
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(0x20000, 0x1000, MemoryProtection::ReadWrite));

    const u8 code[] = {
        0xD1, 0xE8,
        0x21, 0xC0,
        0x74, 0x05,
        0xB8, 0x11, 0x00, 0x00, 0x00,
        0xC3,
        0xB8, 0x22, 0x00, 0x00, 0x00,
        0xC3,
    };
    ASSERT_TRUE(memory.write_bytes(0x1000, code, sizeof(code)));

    for (u32 eax0 : {0u, 1u, 2u, 0x80000000u, 0xFFFFFFFFu}) {
        CpuState ii;
        ii.init_protected_mode();
        ii.gpr.set32(0, eax0);
        ii.gpr.set32(4, 0x20800);
        ii.rip = 0x1000;
        ASSERT_TRUE(memory.write_u32(0x20800, 0));
        {
            Interpreter interp(ii, memory);
            for (int i = 0; i < 32 && ii.rip != 0; ++i) {
                if (interp.step() != ExecResult::Continue) break;
            }
        }

        CpuState ji;
        ji.init_protected_mode();
        ji.gpr.set32(0, eax0);
        ji.gpr.set32(4, 0x20800);
        ji.rip = 0x1000;
        ASSERT_TRUE(memory.write_u32(0x20800, 0));
        {
            JitCompiler jit(memory);
            for (int guard = 0; guard < 16 && ji.rip != 0; ++guard) {
                u8 local[32];
                for (int k = 0; k < 32; ++k) {
                    auto r = memory.read_u8(
                        static_cast<GuestAddr>(ji.rip + k));
                    local[k] = r.is_ok() ? r.value() : 0xCC;
                }
                BasicBlock block;
                block.start_addr = static_cast<GuestAddr>(ji.rip);
                block.is_64bit = false;
                MemoryByteReader reader(local, sizeof(local));
                while (reader.remaining() > 0) {
                    DecodedInstruction inst;
                    if (decode_instruction(reader, DecodeMode::Mode32, inst)
                        != DecodeError::Success) {
                        break;
                    }
                    block.instructions.push_back(inst);
                    const char* nm = opcode_name(inst.opcode);
                    if (nm && (std::strncmp(nm, "J", 1) == 0 ||
                               std::strncmp(nm, "RET", 3) == 0)) {
                        break;
                    }
                }
                block.byte_length = 0;
                for (const auto& in : block.instructions)
                    block.byte_length += in.length;
                block.instruction_count =
                    static_cast<u32>(block.instructions.size());
                if (block.instruction_count == 0) break;
                CompiledBlock* cb = jit.compile(block);
                if (!cb) {
                    Interpreter interp(ji, memory);
                    if (interp.step() != ExecResult::Continue) break;
                    continue;
                }
                jit.execute(*cb, ji);
            }
        }

        EXPECT_EQ(ji.gpr.get32(0), ii.gpr.get32(0)) << "eax0=" << eax0;
        ji.materialize_lazy_flags();
        ii.materialize_lazy_flags();
        EXPECT_EQ(ji.flags.rflags & 0x8D5u, ii.flags.rflags & 0x8D5u)
            << "arith flags mismatch eax0=" << eax0;
    }
}

}  // namespace

// The cold-stub re-emit only works when the Jcc is immediately after its
// publisher: nothing has run in between, so the operand registers are still
// intact at the branch. Put an instruction between them and the store must be
// kept inline, or the taken edge loses EFLAGS.
TEST(FlagLivenessTest, JccNotAdjacentKeepsStore) {
    // CMP eax,ebx; MOV ecx,edx; JB +0x11F; CMP eax,ebx
    // The JB escapes the block and is NOT adjacent to the first CMP.
    const u8 code[] = {
        0x39, 0xD8,
        0x89, 0xD1,
        0x0F, 0x82, 0x1F, 0x01, 0x00, 0x00,
        0x39, 0xD8,
    };
    BasicBlock b = decode_bytes(code, sizeof(code), false);
    ASSERT_EQ(b.instruction_count, 4u);
    std::vector<u8> omit;
    compute_omit_lazy_flags(b, omit);
    EXPECT_EQ(omit[0], 0);  // not adjacent to the escaping JB — store stays
    EXPECT_EQ(omit[3], 0);  // last writer, live-out
}

// SUB RAX,RCX; ADC RAX,RCX; ADD RAX,1 (x64). The SUB's only possible reader
// is the adjacent ADC, which takes CF from the SUB's live NZCV; the ADC's own
// record is retired by the adjacent register-only ADD. Both omissions are
// mode 2 so the CF/OF overlay is still cleared.
TEST(FlagLivenessTest, X64AdjacentCarryPairOmitsBothLinks) {
    const u8 code[] = {0x48, 0x29, 0xC8, 0x48, 0x11, 0xC8, 0x48, 0x83, 0xC0,
                       0x01};
    BasicBlock b = decode_bytes(code, sizeof(code), true);
    ASSERT_EQ(b.instruction_count, 3u);
    std::vector<u8> omit;
    compute_omit_lazy_flags(b, omit);
    ASSERT_EQ(omit.size(), 3u);
    EXPECT_EQ(omit[0], 2);
    EXPECT_EQ(omit[1], 2);
    EXPECT_EQ(omit[2], 0);  // live-out
}

// SAR RAX,1; JS +3; SUB RAX,RCX; ADC RAX,RCX; ... (x64, fuzzer seed 729027).
// The taken JS lands on the ADC, so on that path CF comes from SAR and the
// ADC reads it from the lazy record. The SUB's record must not be omitted
// on the strength of adjacency, and the chain must not extend across the
// join.
TEST(FlagLivenessTest, X64CarryConsumerAtBranchTargetKeepsPublisherRecord) {
    const u8 code[] = {0x48, 0xC1, 0xF8, 0x01, 0x78, 0x03, 0x48, 0x29, 0xC8,
                       0x48, 0x11, 0xC8, 0x48, 0x0F, 0xA4, 0xC8, 0x20};
    BasicBlock b = decode_bytes(code, sizeof(code), true);
    ASSERT_EQ(b.instruction_count, 5u);
    std::vector<u8> omit;
    compute_omit_lazy_flags(b, omit);
    ASSERT_EQ(omit.size(), 5u);
    EXPECT_EQ(omit[0], 0) << "SAR's CF is read by the ADC on the taken path";
    EXPECT_EQ(omit[2], 0) << "SUB's record is the ADC's CF on the fall-through";
}

// Runs `code` at 0x1000 in long mode from the given GPRs (RAX..R15) and
// RFLAGS until RIP reaches its end. A Jcc the JIT does not keep in-block
// exits to its target; the remainder is dispatched from there, as the run
// loop would.
static void run_x64_block_to_end(const u8* code, usize len,
                                 const std::array<u64, 16>& gpr, u64 rflags,
                                 CpuState& cpu) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x1000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.write_bytes(0x1000, code, len));
    cpu.init_long_mode();
    cpu.rip = 0x1000;
    for (u8 r = 0; r < 16; ++r) cpu.gpr.set64(r, gpr[r]);
    cpu.flags.rflags = rflags;
    JitCompiler jit(memory);
    const u64 end = 0x1000u + len;
    for (int guard = 0; guard < 8 && cpu.rip != end; ++guard) {
        const usize off = static_cast<usize>(cpu.rip - 0x1000);
        ASSERT_LT(off, len);
        BasicBlock part = decode_bytes(code + off, len - off, true);
        part.start_addr = cpu.rip;
        CompiledBlock* cb = jit.compile(part);
        ASSERT_NE(cb, nullptr);
        ASSERT_EQ(jit.execute(*cb, cpu), JitExecResult::Continue);
    }
    ASSERT_EQ(cpu.rip, end);
    cpu.materialize_lazy_flags();
}

// Same block end to end. Expected state is Rosetta's measurement recorded by
// the flag-block fuzzer for seed 729027.
TEST(FlagLivenessTest, X64CarryConsumerAtBranchTargetMatchesOracle) {
    const u8 code[] = {
        0x48, 0xC1, 0xF8, 0x01,              // sar rax,1
        0x78, 0x03,                          // js +3
        0x48, 0x29, 0xC8,                    // sub rax,rcx
        0x48, 0x11, 0xC8,                    // adc rax,rcx
        0x48, 0x0F, 0xA4, 0xC8, 0x20,        // shld rax,rcx,32
        0x79, 0x03,                          // jns +3
        0x48, 0x29, 0xC8,                    // sub rax,rcx
        0x48, 0x0F, 0xAC, 0xC8, 0x20,        // shrd rax,rcx,32
    };
    std::array<u64, 16> gpr{};
    gpr[0] = 0x00008fc8ee5d30e4ull;
    gpr[1] = 0x0000a4194ec54e9eull;
    CpuState cpu;
    run_x64_block_to_end(code, sizeof(code), gpr, 0xA57, cpu);
    if (HasFatalFailure()) return;
    EXPECT_EQ(cpu.gpr.get64(0), 0x4ec54e9e772e9873ull);
    EXPECT_EQ(cpu.gpr.get64(1), 0x0000a4194ec54e9eull);
    // SHRD by 32 leaves AF and OF undefined (Intel SDM Vol. 2, SHRD).
    EXPECT_EQ(cpu.flags.rflags & 0xC5u, 0x202u & 0xC5u);
}

// The two blocks that kept full x64 liveness off (fuzzer seeds 729195 and
// 729288, tools/fuzz/repros/). Both end in SBB r64 reading CF across
// spliced forward Jccs. Expected state is the recorded Rosetta result.
TEST(FlagLivenessTest, X64SbbAfterRclAcrossJccsMatchesOracle) {
    const u8 code[] = {
        0x48, 0xD3, 0xC8,                    // ror rax,cl
        0x48, 0xD1, 0xD0,                    // rcl rax,1
        0x78, 0x06,                          // js +6
        0x48, 0x0F, 0xAF, 0xC1,              // imul rax,rcx
        0x73, 0x03,                          // jae +3
        0x48, 0x01, 0xC8,                    // add rax,rcx
        0x4D, 0x1B, 0xFA,                    // sbb r15,r10
    };
    const std::array<u64, 16> gpr = {
        0x0000c181e07eaa73ull, 0x00005fc576c2d686ull, 0x00003fee2f812173ull,
        0x0000295e79c8900aull, 0, 0x0000ef18d03e7eacull, 0x00007f9a5cd7ef2cull,
        0x0000000004000000ull, 0x000072247881ed07ull, 0x0000af0d19610e0full,
        0x000067d239c4db5eull, 0x0000ebbc770b079eull, 0x0000e62c0b3604e5ull,
        0x0000905193e4ea5cull, 0x00004703a9c0c2e5ull, 0x00003077ebef0f40ull,
    };
    CpuState cpu;
    run_x64_block_to_end(code, sizeof(code), gpr, 0x253, cpu);
    if (HasFatalFailure()) return;
    EXPECT_EQ(cpu.gpr.get64(0), 0xbf366895cccea1f8ull);
    EXPECT_EQ(cpu.gpr.get64(15), 0xffffc8a5b22a33e2ull);
    EXPECT_EQ(cpu.flags.rflags & 0x8D5u, 0x297u & 0x8D5u);
}

TEST(FlagLivenessTest, X64SbbAfterCmovAcrossJccsMatchesOracle) {
    const u8 code[] = {
        0x48, 0x01, 0xC8,                    // add rax,rcx
        0x7F, 0x0A,                          // jg +10
        0x48, 0x0F, 0x4B, 0xC1,              // cmovnp rax,rcx
        0x72, 0x0F,                          // jb +15
        0x48, 0x0F, 0x43, 0xC1,              // cmovae rax,rcx
        0x48, 0xD3, 0xF8,                    // sar rax,cl
        0x7A, 0x03,                          // jp +3
        0x4D, 0x01, 0xC8,                    // add r8,r9
        0x48, 0x1B, 0xC1,                    // sbb rax,rcx
    };
    const std::array<u64, 16> gpr = {
        0x0000945a0a03f610ull, 0x0000f74e0d664c9dull, 0x0000ac696f4548edull,
        0x00000accfa17ab2bull, 0, 0x0000a6681635880full, 0x000033c11db125b6ull,
        0x0000000004000000ull, 0x0000119bfcca01bfull, 0x0000bbc5ba0318baull,
        0x0000881345472b15ull, 0x00008a68879ab056ull, 0x00007af85109ccebull,
        0x00002713c5ccf924ull, 0x000046143488e839ull, 0x000098272debf791ull,
    };
    CpuState cpu;
    run_x64_block_to_end(code, sizeof(code), gpr, 0x257, cpu);
    if (HasFatalFailure()) return;
    EXPECT_EQ(cpu.gpr.get64(0), 0xffff08b1f2a610a3ull);
    EXPECT_EQ(cpu.gpr.get64(8), 0x0000cd61b6cd1a79ull);
    EXPECT_EQ(cpu.flags.rflags & 0x8D5u, 0x297u & 0x8D5u);
}

// SBB r/m64 lowered natively: DEST := DEST - (SRC + CF) (Intel SDM Vol. 2,
// SBB), CF = borrow, OF = signed overflow, SF/ZF/PF from the result, AF =
// borrow out of bit 3. CF comes from the preceding CMP either through its
// live NZCV (adjacent) or through its lazy record (a MOV in between).
TEST(FlagLivenessTest, X64SbbRegisterMatchesSdmDefinition) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x1000, MemoryProtection::ReadWrite));
    // cmp rdx,rcx ; [mov r8,r9] ; sbb rax,rbx
    const std::vector<u8> adjacent = {0x48, 0x39, 0xCA, 0x48, 0x19, 0xD8};
    const std::vector<u8> separated = {0x48, 0x39, 0xCA, 0x4D, 0x89, 0xC8,
                                       0x48, 0x19, 0xD8};
    struct Case { u64 rax, rbx, rdx, rcx; };
    const Case cases[] = {
        {5, 3, 1, 2},                                   // CF in = 1
        {5, 3, 2, 1},                                   // CF in = 0
        {0, 0xFFFFFFFFFFFFFFFFull, 0, 1},               // SRC + CF wraps
        {0x8000000000000000ull, 0, 0, 1},               // OF via carry-in
        {0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, 1, 1},
        {0x10, 0x0F, 0, 1},                             // ZF, AF
        {1, 1, 3, 3},
    };
    for (const auto* code : {&adjacent, &separated}) {
        ASSERT_TRUE(memory.write_bytes(0x1000, code->data(), code->size()));
        for (const Case& c : cases) {
            SCOPED_TRACE(testing::Message()
                         << (code == &adjacent ? "adjacent" : "separated")
                         << " rax=" << c.rax << " rbx=" << c.rbx
                         << " rdx=" << c.rdx << " rcx=" << c.rcx);
            BasicBlock b = decode_bytes(code->data(), code->size(), true);
            CpuState cpu;
            cpu.init_long_mode();
            cpu.rip = 0x1000;
            cpu.gpr.set64(0, c.rax);
            cpu.gpr.set64(3, c.rbx);
            cpu.gpr.set64(2, c.rdx);
            cpu.gpr.set64(1, c.rcx);
            JitCompiler jit(memory);
            CompiledBlock* cb = jit.compile(b);
            ASSERT_NE(cb, nullptr);
            ASSERT_EQ(jit.execute(*cb, cpu), JitExecResult::Continue);
            cpu.materialize_lazy_flags();

            const u64 cf_in = c.rdx < c.rcx ? 1 : 0;
            const u64 res = c.rax - c.rbx - cf_in;
            const bool cf = static_cast<unsigned __int128>(c.rbx) + cf_in >
                            c.rax;
            const bool of = (((c.rax ^ c.rbx) & (c.rax ^ res)) >> 63) != 0;
            const bool af = ((c.rax ^ c.rbx ^ res) & 0x10) != 0;
            const bool pf = (__builtin_popcount(static_cast<u8>(res)) & 1) == 0;
            const u32 expect = (cf ? 0x001u : 0) | (pf ? 0x004u : 0) |
                               (af ? 0x010u : 0) | (res == 0 ? 0x040u : 0) |
                               ((res >> 63) ? 0x080u : 0) | (of ? 0x800u : 0);
            EXPECT_EQ(cpu.gpr.get64(0), res);
            EXPECT_EQ(cpu.flags.rflags & 0x8D5u, expect);
        }
    }
}

TEST(FlagLivenessTest, DeadIncDecRequiresAdjacentNonfaultingFullWriter) {
    const std::vector<std::vector<u8>> tails{
        {0x39, 0xd2},             // cmp edx,edx: all flags replaced
        {0x39, 0x10},             // cmp [eax],edx: may fault
        {0x11, 0xd2},             // adc edx,edx: reads carry
        {0x74, 0x00},             // branch: observes flags
        {0x90, 0x39, 0xd2},       // no proof across intervening instructions
        {0xf0, 0x01, 0xd2},       // LOCK register: #UD before replacement
        {},                       // live out
    };
    for (bool wide : {false, true}) for (bool decrement : {false, true}) {
        for (usize i = 0; i < tails.size(); ++i) {
            std::vector<u8> code{0xff, static_cast<u8>(decrement ? 0xc8 : 0xc0)};
            code.insert(code.end(), tails[i].begin(), tails[i].end());
            auto block = decode_bytes(code.data(), code.size(), wide);
            std::vector<u8> omit;
            compute_omit_lazy_flags(block, omit);
            ASSERT_FALSE(omit.empty());
            EXPECT_EQ(omit[0], i == 0 ? 1 : 0) << wide << " " << decrement << " " << i;
        }
        for (const std::vector<u8>& code : {
                std::vector<u8>{0xff, 0x00, 0x39, 0xd2}, // memory INC
                std::vector<u8>{0xf0, 0xff, 0x00, 0x39, 0xd2}}) {
            auto block = decode_bytes(code.data(), code.size(), wide);
            std::vector<u8> omit;
            compute_omit_lazy_flags(block, omit);
            ASSERT_FALSE(omit.empty());
            EXPECT_EQ(omit[0], 0);
        }
    }
}

TEST(FlagLivenessTest, DeadIncDecValuesMatchSdmAcrossWidthsAndModes) {
    // Intel SDM INC/DEC: result +/- 1 at operand width. CMP edx,edx then
    // defines ZF/PF=1 and CF/OF/SF/AF=0, irrespective of the preserved carry.
    struct Form { std::vector<u8> bytes; unsigned width; u8 reg; bool high; bool only64; bool only32; };
    const std::vector<Form> forms{
        {{0xfe, 0xc0}, 8, 0, false, false, false},
        {{0xfe, 0xc4}, 8, 0, true, false, false},   // AH
        {{0x40, 0xfe, 0xc4}, 8, 4, false, true, false}, // SPL
        {{0x41, 0xfe, 0xc0}, 8, 8, false, true, false},
        {{0x66, 0xff, 0xc0}, 16, 0, false, false, false},
        {{0xff, 0xc0}, 32, 0, false, false, false},
        {{0x48, 0xff, 0xc0}, 64, 0, false, true, false},
        {{0x66, 0x40}, 16, 0, false, false, true},
        {{0x40}, 32, 0, false, false, true},
    };
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    JitCompiler jit(memory);
    usize index = 0;
    for (bool wide : {false, true}) for (const auto& form : forms) {
        if ((form.only64 && !wide) || (form.only32 && wide)) continue;
        for (bool decrement : {false, true}) {
            auto code = form.bytes;
            if (decrement) code.back() += 8; // SDM opcode +rd or ModRM /1
            code.insert(code.end(), {0x39, 0xd2});
            auto block = decode_bytes(code.data(), code.size(), wide);
            ASSERT_EQ(block.instruction_count, 2u);
            block.start_addr = 0x1000 + index++ * 0x40;
            ASSERT_TRUE(memory.write_bytes(block.start_addr, code.data(), code.size()));
            auto* compiled = jit.compile(block);
            ASSERT_NE(compiled, nullptr);
            for (u64 value : {u64{0}, u64{0xffffffffffffffff},
                              u64{0x123456787fffffff}, u64{0x1234567880000000}}) {
                for (u64 carry : {u64{0}, u64{1}}) {
                    CpuState cpu;
                    if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
                    for (u8 r = 0; r < 16; ++r) cpu.gpr.set64(r, wide ? value : u32(value));
                    cpu.rip = block.start_addr;
                    cpu.flags.rflags = 0x202 | carry;
                    const auto initial = cpu;
                    const u64 mask = form.width == 64 ? ~u64{0} : (u64{1} << form.width) - 1;
                    const unsigned shift = form.high ? 8 : 0;
                    const u64 old = initial.gpr.get64(form.reg);
                    const u64 result = (((old >> shift) & mask) + (decrement ? ~u64{0} : 1)) & mask;
                    const u64 expected = form.width == 32 ? result :
                        (old & ~(mask << shift)) | (result << shift);
                    ASSERT_EQ(jit.execute(*compiled, cpu), JitExecResult::Continue);
                    EXPECT_FALSE(cpu.pending_raise.active);
                    EXPECT_EQ(cpu.rip, block.start_addr + code.size());
                    for (u8 r = 0; r < 16; ++r)
                        EXPECT_EQ(cpu.gpr.get64(r), r == form.reg ? expected : initial.gpr.get64(r));
                    cpu.materialize_lazy_flags();
                    EXPECT_EQ(cpu.flags.rflags & 0x8d5, 0x44u);
                }
            }
        }
    }
}

// COMISS/UCOMISS/COMISD/UCOMISD read no flag and write all six: ZF/PF/CF from
// the compare, OF/SF/AF cleared (Intel SDM Vol. 2). A register-form compare
// cannot fault, so it retires the record of the ADD before it.
TEST(FlagLivenessTest, RegisterComissKillsPriorAdd) {
    // add edx, 1 ; <compare> xmm0, xmm6 ; cmova eax, edx ; ret
    const std::array<std::array<u8, 4>, 4> compares = {{
        {0x0F, 0x2F, 0xC6, 0x90},        // comiss xmm0, xmm6
        {0x0F, 0x2E, 0xC6, 0x90},        // ucomiss xmm0, xmm6
        {0x66, 0x0F, 0x2F, 0xC6},        // comisd xmm0, xmm6
        {0x66, 0x0F, 0x2E, 0xC6},        // ucomisd xmm0, xmm6
    }};
    for (const bool is_64bit : {false, true}) {
        for (const auto& cmp : compares) {
            SCOPED_TRACE(::testing::Message() << (is_64bit ? "x64 " : "x86 ")
                                              << std::hex << int(cmp[1])
                                              << int(cmp[2]));
            std::vector<u8> code = {0x83, 0xC2, 0x01};
            const usize n = cmp[0] == 0x66 ? 4 : 3;
            code.insert(code.end(), cmp.begin(), cmp.begin() + n);
            code.insert(code.end(), {0x0F, 0x47, 0xC2, 0xC3});
            BasicBlock b = decode_bytes(code.data(), code.size(), is_64bit);
            ASSERT_EQ(b.instruction_count, 4u);
            std::vector<u8> omit;
            compute_omit_lazy_flags(b, omit);
            EXPECT_EQ(omit[0], 1) << "the ADD's record is dead";
        }
    }
}

// A memory-form compare can fault before it writes a flag, and the exception
// context must then hold the ADD's flags: it retires the ADD's record only
// under a recovery plan that covers its own access.
TEST(FlagLivenessTest, MemoryComissKillsPriorAddOnlyWithRecoveryPlan) {
    // add edx, 1 ; comiss xmm0, [rbx] ; cmova eax, edx ; ret
    const u8 code[] = {0x83, 0xC2, 0x01, 0x0F, 0x2F, 0x03,
                       0x0F, 0x47, 0xC2, 0xC3};
    for (const bool is_64bit : {false, true}) {
        SCOPED_TRACE(is_64bit ? "x64" : "x86");
        BasicBlock b = decode_bytes(code, sizeof(code), is_64bit);
        ASSERT_EQ(b.instruction_count, 4u);
        std::vector<u8> omit;
        std::vector<StencilFlagRecovery> recovery;
        compute_omit_lazy_flags(b, omit, &recovery);
        EXPECT_EQ(omit[0], 1);
        ASSERT_EQ(recovery.size(), 1u);
        EXPECT_EQ(recovery[0].width, 32u);
        EXPECT_EQ(recovery[0].first_rip, 0x1003u);
        EXPECT_EQ(recovery[0].last_rip, 0x1003u);
    }
}

// A full writer with a memory operand can fault before writing any flag, which
// exposes the record of the writer before it (Intel SDM Vol. 3A 6.6). It may
// retire that record only when a recovery plan covers its own access.
TEST(FlagLivenessTest, AdjacentMemoryKillerOmitsOnlyWithRecoveryPlan) {
    // add eax, 1 ; cmp dword [ebx], 0 ; jz +0
    const u8 code[] = {0x83, 0xC0, 0x01, 0x83, 0x3B, 0x00, 0x74, 0x00};
    BasicBlock b = decode_bytes(code, sizeof(code), false);
    ASSERT_EQ(b.instruction_count, 3u);
    std::vector<u8> omit;
    std::vector<StencilFlagRecovery> recovery;
    compute_omit_lazy_flags(b, omit, &recovery);
    EXPECT_EQ(omit[0], 1);
    ASSERT_EQ(recovery.size(), 1u);
    EXPECT_EQ(recovery[0].flag_op, static_cast<u8>(FlagState::FlagOp::Add32));
    EXPECT_EQ(recovery[0].def_rip, 0x1000u);
    EXPECT_EQ(recovery[0].first_rip, 0x1003u);  // the CMP's own access
    EXPECT_EQ(recovery[0].last_rip, 0x1003u);
}

TEST(FlagLivenessTest, X64MemoryKillerRetiresTheRecordUnderARecoveryPlan) {
    // add eax, 1 ; cmp dword [rbx], 0 ; jz +0 -- as on x86, a 32-bit record.
    const u8 code[] = {0x83, 0xC0, 0x01, 0x83, 0x3B, 0x00, 0x74, 0x00};
    BasicBlock b = decode_bytes(code, sizeof(code), true);
    ASSERT_EQ(b.instruction_count, 3u);
    std::vector<u8> omit;
    std::vector<StencilFlagRecovery> recovery;
    compute_omit_lazy_flags(b, omit, &recovery);
    EXPECT_EQ(omit[0], 1);
    ASSERT_EQ(recovery.size(), 1u);
    EXPECT_EQ(recovery[0].flag_op, static_cast<u8>(FlagState::FlagOp::Add32));
    EXPECT_EQ(recovery[0].width, 32u);
    EXPECT_FALSE(recovery[0].recompute_dst);
    EXPECT_EQ(recovery[0].dst_reg, 0u);
    EXPECT_TRUE(recovery[0].src_is_imm);
    EXPECT_EQ(recovery[0].src_imm, 1u);
    EXPECT_EQ(recovery[0].first_rip, 0x1003u);
    EXPECT_EQ(recovery[0].last_rip, 0x1003u);
}

// A register SETcc / CMOVcc whose condition the publisher's host NZCV answers
// reads NZCV, not the lazy record, writes no flag and cannot fault (Intel SDM
// Vol. 2 SETcc, CMOVcc: Flags Affected = None), so the scan crosses it to the
// killer. Conditions NZCV cannot answer for that publisher (PF always; CF and
// CF|ZF after TEST/AND, whose host C is not x86 CF; BE/A after ADD), OR/XOR
// and 16-bit publishers (no host NZCV), and memory forms keep the record. The
// elided span runs from after the publisher through the killer.
TEST(FlagLivenessTest, NzcvReadersAreCrossedOnlyWhenTheirConditionFuses) {
    struct Case { const char* name; std::vector<u8> code; bool is_64bit; bool omitted; };
    // Each: publisher ; reader(s) ; killer (add esi, 1 / add rsi, 1).
    const Case cases[] = {
        {"cmp; sete; setg", {0x39, 0xC8, 0x0F, 0x94, 0xC0, 0x0F, 0x9F, 0xC2, 0x83, 0xC6, 0x01},
         false, true},
        {"cmp r64; setb r8b; seta r9b", {0x48, 0x39, 0xC8, 0x41, 0x0F, 0x92, 0xC0,
                                         0x41, 0x0F, 0x97, 0xC1, 0x48, 0x83, 0xC6, 0x01},
         true, true},
        {"cmp; cmovl; cmovg", {0x39, 0xC8, 0x0F, 0x4C, 0xC1, 0x0F, 0x4F, 0xD1, 0x83, 0xC6, 0x01},
         false, true},
        {"cmp r64; cmovb r8,rcx", {0x48, 0x39, 0xC8, 0x4C, 0x0F, 0x42, 0xC1,
                                   0x48, 0x83, 0xC6, 0x01}, true, true},
        {"test; sets; setz", {0x85, 0xC0, 0x0F, 0x98, 0xC0, 0x0F, 0x94, 0xC1, 0x83, 0xC6, 0x01},
         false, true},
        {"add; seto; setb", {0x01, 0xC8, 0x0F, 0x90, 0xC0, 0x0F, 0x92, 0xC2, 0x83, 0xC6, 0x01},
         false, true},
        {"cmp; setp", {0x39, 0xC8, 0x0F, 0x9A, 0xC0, 0x83, 0xC6, 0x01}, false, false},
        {"test; setb", {0x85, 0xC8, 0x0F, 0x92, 0xC0, 0x83, 0xC6, 0x01}, false, false},
        {"test; seta", {0x85, 0xC8, 0x0F, 0x97, 0xC0, 0x83, 0xC6, 0x01}, false, false},
        {"add; seta", {0x01, 0xC8, 0x0F, 0x97, 0xC0, 0x83, 0xC6, 0x01}, false, false},
        {"xor; setz", {0x31, 0xC8, 0x0F, 0x94, 0xC0, 0x83, 0xC6, 0x01}, false, false},
        {"cmp r16; setz", {0x66, 0x39, 0xC8, 0x0F, 0x94, 0xC0, 0x83, 0xC6, 0x01}, false, false},
        {"cmp; setz [ebx]", {0x39, 0xC8, 0x0F, 0x94, 0x03, 0x83, 0xC6, 0x01}, false, false},
        {"cmp; cmovz eax,[ebx]", {0x39, 0xC8, 0x0F, 0x44, 0x03, 0x83, 0xC6, 0x01}, false, false},
        {"test al,cl; setnz", {0x84, 0xC8, 0x0F, 0x95, 0xC0, 0x83, 0xC6, 0x01}, false, false},
        {"test al,1; cmovz", {0xA8, 0x01, 0x0F, 0x44, 0xC1, 0x83, 0xC6, 0x01}, false, false},
        {"test ax,cx; setz", {0x66, 0x85, 0xC8, 0x0F, 0x94, 0xC0, 0x83, 0xC6, 0x01}, false, false},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        BasicBlock b = decode_bytes(c.code.data(), c.code.size(), c.is_64bit);
        ASSERT_GE(b.instruction_count, 3u);
        std::vector<u8> omit;
        std::vector<StencilFlagRecovery> recovery;
        std::vector<u32> elided;
        compute_omit_lazy_flags(b, omit, &recovery, &elided);
        EXPECT_EQ(omit[0], c.omitted ? 1 : 0);
        ASSERT_EQ(elided.size(), b.instruction_count);
        EXPECT_EQ(elided[0], 0u);
        for (u32 k = 1; k < b.instruction_count; ++k) {
            EXPECT_EQ(elided[k], c.omitted ? 1u : 0u) << "k=" << k;
        }
    }
}

// A REX.W publisher's plan is a 64-bit record: imm8/imm32 sign-extended to 64
// bits (Intel SDM Vol. 2 CMP/ADD), registers up to R15.
TEST(FlagLivenessTest, X64RexWPublisherPlansA64BitRecord) {
    struct Case { const char* name; std::vector<u8> code; u8 op; bool recompute;
                  bool src_is_imm; u8 src_reg; u64 src_imm; u8 dst_reg; };
    const Case cases[] = {
        // cmp rax, rcx ; mov rdx, [rbx] ; add rsi, 1
        {"cmp rax,rcx", {0x48, 0x39, 0xC8, 0x48, 0x8B, 0x13, 0x48, 0x83, 0xC6, 0x01},
         static_cast<u8>(FlagState::FlagOp::Sub64), true, false, 1, 0, 0},
        // cmp rax, -3 ; mov rdx, [rbx] ; add rsi, 1
        {"cmp rax,-3", {0x48, 0x83, 0xF8, 0xFD, 0x48, 0x8B, 0x13, 0x48, 0x83, 0xC6, 0x01},
         static_cast<u8>(FlagState::FlagOp::Sub64), true, true, 0xFF,
         0xFFFFFFFFFFFFFFFDull, 0},
        // sub r9, r10 ; mov rdx, [rbx] ; add rsi, 1
        {"sub r9,r10", {0x4D, 0x29, 0xD1, 0x48, 0x8B, 0x13, 0x48, 0x83, 0xC6, 0x01},
         static_cast<u8>(FlagState::FlagOp::Sub64), false, false, 10, 0, 9},
        // test rax, 0x80000000 (sign-extended) ; mov rdx, [rbx] ; add rsi, 1
        {"test rax,imm32", {0x48, 0xA9, 0x00, 0x00, 0x00, 0x80, 0x48, 0x8B, 0x13,
                            0x48, 0x83, 0xC6, 0x01},
         static_cast<u8>(FlagState::FlagOp::And64), true, true, 0xFF,
         0xFFFFFFFF80000000ull, 0},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        BasicBlock b = decode_bytes(c.code.data(), c.code.size(), true);
        ASSERT_EQ(b.instruction_count, 3u);
        std::vector<u8> omit;
        std::vector<StencilFlagRecovery> recovery;
        compute_omit_lazy_flags(b, omit, &recovery);
        EXPECT_EQ(omit[0], 1);
        ASSERT_EQ(recovery.size(), 1u);
        const StencilFlagRecovery& p = recovery[0];
        EXPECT_EQ(p.flag_op, c.op);
        EXPECT_EQ(p.width, 64u);
        EXPECT_EQ(p.recompute_dst, c.recompute);
        EXPECT_EQ(p.src_is_imm, c.src_is_imm);
        if (c.src_is_imm) EXPECT_EQ(p.src_imm, c.src_imm);
        else              EXPECT_EQ(p.src_reg, c.src_reg);
        EXPECT_EQ(p.dst_reg, c.dst_reg);
        EXPECT_EQ(p.first_rip, 0x1000u + b.instructions[0].length);
    }
}

// A plan rebuilds the record from its operand registers at the fault, so any
// instruction before the fault that writes one -- explicitly through operand
// 0, through XCHG's second operand, or implicitly (CDQ: EDX, PUSH/POP: RSP,
// one-operand MUL: EDX:EAX, string instructions: RSI/RDI/RCX) -- must keep the
// record, while writes to other registers must not. ADD with one register as
// both operands keeps it too: the register then holds the result.
TEST(FlagLivenessTest, RecoveryPlanRefusesEveryWriteToAnOperandRegister) {
    struct Case { const char* name; std::vector<u8> code; bool is_64bit; bool omitted; };
    // Each: publisher ; middle ; mov edx/rdx, [ebx/rbx] ; add esi, 1.
    const Case cases[] = {
        {"cmp eax,ecx; cdq", {0x39, 0xC8, 0x99, 0x8B, 0x13, 0x83, 0xC6, 0x01}, false, true},
        {"cmp eax,edx; cdq", {0x39, 0xD0, 0x99, 0x8B, 0x0B, 0x83, 0xC6, 0x01}, false, false},
        {"cmp eax,ecx; xchg edi,ecx", {0x39, 0xC8, 0x87, 0xCF, 0x8B, 0x13, 0x83, 0xC6, 0x01},
         false, false},
        {"cmp eax,ecx; xchg edi,ebp", {0x39, 0xC8, 0x87, 0xEF, 0x8B, 0x13, 0x83, 0xC6, 0x01},
         false, true},
        {"cmp esp,ecx; push ebx", {0x39, 0xCC, 0x53, 0x8B, 0x13, 0x83, 0xC6, 0x01}, false, false},
        {"cmp eax,ecx; mul ebx", {0x39, 0xC8, 0xF7, 0xE3, 0x8B, 0x13, 0x83, 0xC6, 0x01},
         false, false},
        {"cmp esi,ecx; movsb", {0x39, 0xCE, 0xA4, 0x8B, 0x13, 0x83, 0xC2, 0x01}, false, false},
        {"cmp rax,rcx; pop rcx", {0x48, 0x39, 0xC8, 0x59, 0x48, 0x8B, 0x13, 0x48, 0x83, 0xC6, 0x01},
         true, false},
        {"cmp rax,rcx; pop rdi", {0x48, 0x39, 0xC8, 0x5F, 0x48, 0x8B, 0x13, 0x48, 0x83, 0xC6, 0x01},
         true, true},
        {"cmp rax,rcx; mov ch,dl", {0x48, 0x39, 0xC8, 0x88, 0xD5, 0x48, 0x8B, 0x13,
                                    0x48, 0x83, 0xC6, 0x01}, true, false},
        {"add rax,rax", {0x48, 0x01, 0xC0, 0x90, 0x48, 0x8B, 0x13, 0x48, 0x83, 0xC6, 0x01},
         true, false},
        {"sub rax,rax", {0x48, 0x29, 0xC0, 0x90, 0x48, 0x8B, 0x13, 0x48, 0x83, 0xC6, 0x01},
         true, true},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        BasicBlock b = decode_bytes(c.code.data(), c.code.size(), c.is_64bit);
        ASSERT_EQ(b.instruction_count, 4u);
        std::vector<u8> omit;
        std::vector<StencilFlagRecovery> recovery;
        compute_omit_lazy_flags(b, omit, &recovery);
        EXPECT_EQ(omit[0], c.omitted ? 1 : 0);
        EXPECT_EQ(recovery.size(), c.omitted ? 1u : 0u);
    }
}

// ---------------------------------------------------------------------------
// The delegate (jit_interpret_decoded) drops a pending lazy-flag record only
// when the delegated instruction redefines every flag, reads none and cannot
// fault. classify() answers from a per-opcode table; these pin the observable
// RFLAGS around a delegation to the Intel SDM Vol. 2 values.
// ---------------------------------------------------------------------------
#include "core/cpu/jit/jit_helpers.hpp"

namespace {

struct DelegateFixture {
    GuestMemory mem;
    CpuState cpu;
    DelegateFixture() {
        EXPECT_TRUE(mem.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        cpu.init_long_mode();
        cpu.gpr.set64(4, 0x10000);
        // Pending record of SUB r64: 1 - 2. SDM SUB: result 0xFFFF_FFFF_FFFF_FFFF,
        // CF=1 (borrow), ZF=0, SF=1, OF=0, PF=1 (low byte 0xFF), AF=1.
        cpu.flags.rflags = 0x202;
        cpu.flag_op = CpuState::FlagOp::Sub64;
        cpu.flag_src = 2;
        cpu.flag_dst = ~u64{0};
    }
    DecodedInstruction decode(std::initializer_list<u8> bytes) {
        const std::vector<u8> v(bytes);
        BasicBlock b = decode_bytes(v.data(), v.size(), true);
        EXPECT_EQ(b.instruction_count, 1u);
        return b.instructions[0];
    }
    void expect_sub_1_minus_2_flags() {
        cpu.materialize_lazy_flags();
        EXPECT_TRUE(cpu.flags.cf());
        EXPECT_FALSE(cpu.flags.zf());
        EXPECT_TRUE(cpu.flags.sf());
        EXPECT_FALSE(cpu.flags.of());
        EXPECT_TRUE(cpu.flags.pf());
        EXPECT_TRUE(cpu.flags.af());
    }
};

}  // namespace

TEST(DelegateFlagState, NonWriterKeepsPendingRecord) {
    DelegateFixture f;
    // VEXTRACTF128 xmm0, ymm1, 1: Flags Affected none (SDM).
    const DecodedInstruction in = f.decode({0xC4, 0xE3, 0x7D, 0x19, 0xC8, 0x01});
    EXPECT_FALSE(pending_flags_dead_across(in));
    f.cpu.avx.ymm_high[1].u64_[0] = 0x1122334455667788ull;
    jit_interpret_decoded(&f.cpu, &f.mem, &in, 0x1000);
    EXPECT_EQ(f.cpu.sse.xmm[0].u64_[0], 0x1122334455667788ull);
    f.expect_sub_1_minus_2_flags();
}

TEST(DelegateFlagState, RegisterFullWriterReplacesRecord) {
    DelegateFixture f;
    // ADD rax, rcx with rax=1, rcx=~0: SDM ADD gives 0 with CF=1, ZF=1, SF=0,
    // OF=0, PF=1, AF=1 (carry out of bit 3).
    const DecodedInstruction in = f.decode({0x48, 0x01, 0xC8});
    EXPECT_TRUE(pending_flags_dead_across(in));
    f.cpu.gpr.rax = 1;
    f.cpu.gpr.rcx = ~u64{0};
    jit_interpret_decoded(&f.cpu, &f.mem, &in, 0x1000);
    f.cpu.materialize_lazy_flags();
    EXPECT_EQ(f.cpu.gpr.rax, 0u);
    EXPECT_TRUE(f.cpu.flags.cf());
    EXPECT_TRUE(f.cpu.flags.zf());
    EXPECT_FALSE(f.cpu.flags.sf());
    EXPECT_FALSE(f.cpu.flags.of());
    EXPECT_TRUE(f.cpu.flags.pf());
    EXPECT_TRUE(f.cpu.flags.af());
}

TEST(DelegateFlagState, FaultingFullWriterKeepsPendingRecord) {
    DelegateFixture f;
    // ADD [rax], rcx to an unmapped address faults before writing a flag, so
    // the exception observes the SUB's flags.
    const DecodedInstruction in = f.decode({0x48, 0x01, 0x08});
    EXPECT_FALSE(pending_flags_dead_across(in));
    f.cpu.gpr.rax = 0x200000;
    jit_interpret_decoded(&f.cpu, &f.mem, &in, 0x1000);
    EXPECT_TRUE(f.cpu.pending_raise.active);
    f.expect_sub_1_minus_2_flags();
}

TEST(DelegateFlagState, ShiftImmediateDependsOnMaskedCount) {
    DelegateFixture f;
    // SHL r/m32, imm8: a masked count of 0 leaves every flag unchanged; any
    // other count defines CF/OF/SF/ZF/PF (SDM SAL/SAR/SHL/SHR). The table
    // must not hide the operand-dependent half of classify().
    EXPECT_FALSE(pending_flags_dead_across(f.decode({0xC1, 0xE0, 0x00})));
    EXPECT_FALSE(pending_flags_dead_across(f.decode({0xC1, 0xE0, 0x20})));
    EXPECT_TRUE(pending_flags_dead_across(f.decode({0xC1, 0xE0, 0x01})));
    const DecodedInstruction zero = f.decode({0xC1, 0xE0, 0x20});
    f.cpu.gpr.rax = 0x80000001u;
    jit_interpret_decoded(&f.cpu, &f.mem, &zero, 0x1000);
    EXPECT_EQ(f.cpu.gpr.get32(0), 0x80000001u);
    f.expect_sub_1_minus_2_flags();
}
