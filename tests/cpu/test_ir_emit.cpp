// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_emit.cpp
/// @brief Tier 1 emitted ARM64 must agree with the interpreter, exactly.
///
/// The interpreter is the semantic oracle, so every case here runs the same
/// guest bytes twice — once interpreted, once through Tier 1 emitted code —
/// and compares the full architectural result: all eight GPRs **and the
/// materialized RFLAGS**.
///
/// Comparing flags is the point, not a bonus. Tier 1 publishes a lazy
/// `FlagState` record whose two payload words mean different things per
/// operation, and the operand order is wrong only for ASYMMETRIC operands —
/// `flag_dst - flag_src` reconstructs the original destination for Add32, so
/// swapping them still produces correct flags whenever the two operands are
/// equal. Every case below therefore uses distinct, asymmetric, sign-crossing
/// operand values, and asserts CF/OF/SF/ZF/AF/PF rather than just the result
/// register.
///
/// These tests drive real emitted code rather than inspecting emitted words:
/// what matters is what the code DOES, and an assertion on instruction
/// encodings would pass a wrong lowering that happened to encode neatly.
///
/// **They only run with the Tier 1 gate on**, and skip otherwise.
/// `JitCompiler::ir_policy()` caches its parse in a `static const` by design —
/// a gate that could change mid-process would let an IR-compiled block link to
/// a stencil block emitted under a different pin ABI — so the enabled arm is
/// only reachable in a process started with `VITREO_JIT_IR` set. The ctest
/// entry `vitreo_cpu_tests_ir_gate_on` is that process. A skip here therefore
/// means "wrong arm", not "nothing to check".

#include "ir_emit_test_support.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_emit;

TEST(IrEmit, AddRegReg) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter({0x01, 0xC8});  // add eax, ecx
}

TEST(IrEmit, SubRegReg) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter({0x29, 0xC8});  // sub eax, ecx
}

TEST(IrEmit, AndOrXor) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter({0x21, 0xC8});  // and eax, ecx
    expect_tier1_matches_interpreter({0x09, 0xC8});  // or  eax, ecx
    expect_tier1_matches_interpreter({0x31, 0xC8});  // xor eax, ecx
}

TEST(IrEmit, AddImmediate) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter({0x83, 0xC0, 0x05});        // add eax, 5
    expect_tier1_matches_interpreter({0x83, 0xE8, 0xFF});        // sub eax, -1
    expect_tier1_matches_interpreter({0x05, 0x78, 0x56, 0x34, 0x12});
}

TEST(IrEmit, CmpDefinesFlagsOnly) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter({0x39, 0xC8});  // cmp eax, ecx
}

TEST(IrEmit, TestDefinesFlagsOnly) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter({0x85, 0xC8});  // test eax, ecx
}

TEST(IrEmit, NotDefinesNoFlags) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter({0xF7, 0xD0});  // not eax
}

TEST(IrEmit, NegDefinesFlagsFromTheOriginal) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter({0xF7, 0xD8});  // neg eax
}

TEST(IrEmit, MovRegAndImmediate) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter({0x89, 0xC8});  // mov eax, ecx
    expect_tier1_matches_interpreter({0xB8, 0x78, 0x56, 0x34, 0x12});
}

TEST(IrEmit, Lea) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter({0x8D, 0x44, 0x0A, 0x10});  // lea eax,[edx+ecx+16]
    expect_tier1_matches_interpreter({0x8D, 0x04, 0x8A});        // lea eax,[edx+ecx*4]
}

// A sequence, so a value that outlived its guest instruction — or a scratch
// register reused across one — would show up as a wrong later result.
TEST(IrEmit, MultiInstructionSequence) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter({
        0x01, 0xC8,        // add eax, ecx
        0x31, 0xD1,        // xor ecx, edx
        0x29, 0xC8,        // sub eax, ecx
        0x83, 0xC0, 0x07,  // add eax, 7
        0x39, 0xD0,        // cmp eax, edx
    });
}

// The flags of an EARLIER instruction must not survive a later flag-defining
// one, and the last writer must win. A wrong operand order in the lazy record
// shows up here even when each instruction alone happens to agree.
TEST(IrEmit, LastFlagWriterWins) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter({
        0x39, 0xC8,        // cmp eax, ecx
        0x01, 0xD0,        // add eax, edx
    });
}

// A block Tier 1 cannot express must still execute correctly, via Tier 0.
// This runs in BOTH arms on purpose: with the gate on it proves the fallback,
// with the gate off it is simply Tier 0 checked against the interpreter.
TEST(IrEmit, DeclinedBlockStillMatchesInterpreterViaTierZero) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    // `shl eax, cl` — a RUNTIME count, which Tier 1 declines because a count
    // of zero modifies no flags. `shl eax, 4` used to serve here and no longer
    // declines, which is the point of keeping this test pinned to something
    // Tier 1 genuinely cannot express.
    const std::vector<u8> code = {0xD3, 0xE0, 0x01, 0xC8};  // shl eax,cl; add eax,ecx
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

    CpuState interp_cpu;
    seed_state(interp_cpu, kSeeds[1]);
    {
        Interpreter interp(interp_cpu, memory);
        for (int i = 0; i < 2; ++i) {
            ASSERT_EQ(interp.step(), ExecResult::Continue);
        }
    }

    CpuState jit_cpu;
    seed_state(jit_cpu, kSeeds[1]);
    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    EXPECT_FALSE(jit.ir_build_block(block).ok) << "shift must decline";

    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr) << "Tier 0 must still compile a declined block";
    jit.execute(*compiled, jit_cpu);

    jit_cpu.materialize_lazy_flags();
    interp_cpu.materialize_lazy_flags();
    for (u8 r = 0; r < 8; ++r) {
        EXPECT_EQ(jit_cpu.gpr.get32(r), interp_cpu.gpr.get32(r)) << "GPR " << int(r);
    }
    EXPECT_EQ(jit_cpu.flags.rflags, interp_cpu.flags.rflags);
}

// Measurement, not just correctness: the flag-demand pass has to actually
// remove stores, and the emitted size is the only honest way to say so. This
// prints the block's emitted word count and asserts the pass's own counter, so
// running the suite under `VITREO_JIT_IR_PASSES=0` and diffing the printed
// number is a one-command A/B.
TEST(IrEmit, FlagDemandPassRemovesEmittedStores) {
    // Deliberately NOT gated: with the gate off this reports Tier 0's number
    // for the same block, which is the only comparison that says whether Tier 1
    // is competitive rather than merely smaller than its own previous self.
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    // Five flag-defining instructions; only the last is live out of the block.
    const std::vector<u8> code = {
        0x01, 0xC8,        // add eax, ecx
        0x31, 0xD1,        // xor ecx, edx
        0x29, 0xC8,        // sub eax, ecx
        0x83, 0xC0, 0x07,  // add eax, 7
        0x39, 0xD0,        // cmp eax, edx
    };
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    ASSERT_TRUE(jit.ir_build_block(block).ok);

    u32 dead = 0, live = 0;
    for (const ir::Inst& in : jit.ir_block().insts) {
        if (in.op != ir::Op::DefFlags) continue;
        (in.flags.repr == ir::FlagRepr::Dead ? dead : live)++;
    }
    ASSERT_EQ(dead + live, 5u) << "five flag definitions expected";

    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    std::printf("[measure] five-ALU block, %s: %u ARM words, %u/%u flag "
                "definitions eliminated\n",
                JitCompiler::ir_enabled_for(false) ? "Tier 1" : "Tier 0",
                compiled->code_size / 4u, dead, dead + live);

    if (!JitCompiler::ir_enabled_for(false)) return;  // Tier 0 arm: report only
    if (ir::pass_enabled(ir::Pass::FlagDemand)) {
        EXPECT_EQ(dead, 4u) << "only the block-final definition is live-out";
        EXPECT_EQ(live, 1u);
    } else {
        EXPECT_EQ(dead, 0u) << "kill switch must disable the pass entirely";
    }
}

// The in-place win, asserted on the emitted words rather than inferred from a
// size total: `add eax, ecx` must become one ARM instruction writing the pin,
// with no scratch round trip.
TEST(IrEmit, AluResultIsComputedStraightIntoTheDestinationPin) {
    REQUIRE_IR_GATE();
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    const std::vector<u8> code = {0x01, 0xC8};  // add eax, ecx
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);

    const u32* words = static_cast<const u32*>(compiled->code);
    const u32 count = compiled->code_size / 4u;
    bool has_inplace = false, has_scratch_roundtrip = false;
    for (u32 i = 0; i < count; ++i) {
        // EAX is X23, ECX is X24 in the fixed 32-bit pin layout.
        if (words[i] == vitreo::cpu::jit::arm64::add_w(23, 23, 24))
            has_inplace = true;
        if (words[i] == vitreo::cpu::jit::arm64::mov_w(23, 0))
            has_scratch_roundtrip = true;
    }
    EXPECT_TRUE(has_inplace) << "result must be computed into the pin";
    EXPECT_FALSE(has_scratch_roundtrip) << "no scratch->pin copy should remain";
}

// ...and the case that must NOT take it. `neg eax` publishes `Neg32`, whose
// materialization reconstructs flags from the ORIGINAL EAX. Computing the
// result into X23 would destroy that original before the flag record reads it
// — the same wrong-flags bug as before, reintroduced by an optimization rather
// than by an ordering. The correctness of this is already covered by
// `NegDefinesFlagsFromTheOriginal`; this asserts the MECHANISM, so a future
// change to the guard fails here loudly instead of silently reopening the bug.
TEST(IrEmit, NegDoesNotTakeTheInPlaceDestination) {
    REQUIRE_IR_GATE();
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));
    const std::vector<u8> code = {0xF7, 0xD8};  // neg eax
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);

    const u32* words = static_cast<const u32*>(compiled->code);
    const u32 count = compiled->code_size / 4u;
    for (u32 i = 0; i < count; ++i) {
        EXPECT_NE(words[i], vitreo::cpu::jit::arm64::sub_w(23, 31, 23))
            << "NEG must not write its own pin before the flag record reads it";
    }
}

// =============================================================================
// Memory (M4)
// =============================================================================

TEST(IrEmit, LoadFromMemory) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter_with_memory({0x8B, 0x01});  // mov eax,[ecx]
}

TEST(IrEmit, StoreToMemory) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter_with_memory({0x89, 0x01});  // mov [ecx],eax
}

TEST(IrEmit, LoadAndStoreWithDisplacement) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter_with_memory({0x8B, 0x41, 0x08});
    expect_tier1_matches_interpreter_with_memory({0x89, 0x41, 0x0C});
}

// A load and a store around ALU: the shape where a scratch-resident address
// would be destroyed by the access if the register discipline were wrong.
TEST(IrEmit, MixedMemoryAndAluBlock) {
    REQUIRE_IR_GATE();
    expect_tier1_matches_interpreter_with_memory({
        0x8B, 0x01,        // mov eax, [ecx]
        0x83, 0xC0, 0x05,  // add eax, 5
        0x89, 0x41, 0x04,  // mov [ecx+4], eax
    });
}

// THE fault test, and the reason the IR carries `guest_rip` on every op.
// A store through an unmapped address must raise a guest access violation
// attributed to the STORE, not to the instruction before it — a fault at the
// wrong RIP is an SEH handler resuming in the wrong place.
TEST(IrEmit, FaultingStoreReportsTheFaultingInstructionsRip) {
    REQUIRE_IR_GATE();
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite));

    // add eax,ecx     at 0x1000 (2 bytes) -- so the fault is NOT at block entry
    // mov [edx],eax   at 0x1002           -- EDX points at nothing
    const std::vector<u8> code = {0x01, 0xC8, 0x89, 0x02};
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(0, 0x11);
    cpu.gpr.set32(1, 0x22);
    cpu.gpr.set32(2, 0xDEAD0000u);
    cpu.gpr.set32(4, 0x20800);
    cpu.rip = kCode;

    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);
    ASSERT_TRUE(jit.ir_build_block(block).ok)
        << ir::decline_reason_name(jit.ir_last_result().reason);

    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_TRUE(jit.ir_emit_active()) << "must be a Tier 1 block";
    jit.execute(*compiled, cpu);

    ASSERT_TRUE(cpu.pending_raise.active) << "the store must raise";
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.args[0], 1u) << "a store is a WRITE fault";
    EXPECT_EQ(cpu.pending_raise.args[1], 0xDEAD0000u) << "faulting address";
    EXPECT_EQ(cpu.rip, kCode + 2)
        << "the fault belongs to the MOV at 0x1002, not the ADD at 0x1000";
    EXPECT_EQ(cpu.gpr.get32(0), 0x33u) << "the ADD before it committed";
}
