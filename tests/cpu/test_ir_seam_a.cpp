// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_seam_a.cpp
/// @brief The Tier 1 boundary: a real `BasicBlock` through the real compiler.
///
/// `test_ir_frontend.cpp` proves the frontend against decoded instructions.
/// This file proves the *wiring* — that `JitCompiler` hands it a well-formed
/// block: the right instruction array, the right count, and above all the
/// right per-instruction guest RIPs, which `DecodedInstruction` does not carry
/// and which the compiler therefore has to re-derive. A wrong address here is
/// not a coverage miss; it is a wrong `guest_rip` on a faulting op, which is a
/// corrupted guest register inside an SEH handler arbitrarily far from the
/// cause.
///
/// The tests call `ir_build_block` rather than going through `compile()`,
/// because `ir_policy()` caches its parse in a `static const` on purpose — the
/// gate cannot change mid-process, so no test can reach the accept path by
/// setting the environment. `ir_build_block` is the gate-free half for exactly
/// this reason; `ir_attempt_block` (the gated half) is one `if` on top of it.

#include <gtest/gtest.h>

#include <string>

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/memory/guest_memory.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

constexpr GuestAddr kStart = 0x1000;

BasicBlock decode_bytes(const u8* code, usize len, bool is_64bit = false) {
    BasicBlock block;
    block.start_addr = kStart;
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

}  // namespace

// A register-only ALU block reaches the frontend and is accepted.
TEST(IrSeamA, AcceptsRegisterOnlyAluBlock) {
    GuestMemory memory;
    JitCompiler jit(memory);

    const u8 code[] = {
        0x01, 0xC8,        // add eax, ecx
        0x31, 0xD2,        // xor edx, edx
        0x39, 0xC8,        // cmp eax, ecx
    };
    BasicBlock block = decode_bytes(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 3u);

    const ir::BuildResult& r = jit.ir_build_block(block);
    EXPECT_TRUE(r.ok) << ir::decline_reason_name(r.reason);

    std::string why;
    EXPECT_TRUE(ir::verify(jit.ir_block(), &why)) << why;
}

// THE point of this file. Every op's `guest_rip` must be the address of the
// guest instruction that owns it, derived by accumulating instruction lengths
// from `block.start_addr`. The instructions below have lengths 2, 2 and 3, so
// the three distinct RIPs must be exactly 0x1000, 0x1002 and 0x1004 — and the
// verifier's ascending-contiguous-run rule would not catch a uniformly wrong
// base.
TEST(IrSeamA, GuestRipsAreTheRealInstructionAddresses) {
    GuestMemory memory;
    JitCompiler jit(memory);

    const u8 code[] = {
        0x01, 0xC8,              // 0x1000, len 2: add eax, ecx
        0x31, 0xD2,              // 0x1002, len 2: xor edx, edx
        0x83, 0xC0, 0x05,        // 0x1004, len 3: add eax, 5
    };
    BasicBlock block = decode_bytes(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 3u);
    ASSERT_EQ(block.instructions[0].length, 2u);
    ASSERT_EQ(block.instructions[1].length, 2u);
    ASSERT_EQ(block.instructions[2].length, 3u);

    ASSERT_TRUE(jit.ir_build_block(block).ok);

    const ir::IrBlock& ir_block = jit.ir_block();
    EXPECT_EQ(ir_block.entry_rip, kStart);
    ASSERT_FALSE(ir_block.empty());

    bool saw[3] = {false, false, false};
    for (const ir::Inst& inst : ir_block.insts) {
        if (inst.guest_rip == kStart + 0) saw[0] = true;
        else if (inst.guest_rip == kStart + 2) saw[1] = true;
        else if (inst.guest_rip == kStart + 4) saw[2] = true;
        else ADD_FAILURE() << "op owned by no guest instruction: rip=0x"
                           << std::hex << inst.guest_rip;
    }
    EXPECT_TRUE(saw[0] && saw[1] && saw[2])
        << "every guest instruction must own at least one op";
}

// A block whose decoded array was released (`publish_compiled` does this once
// no generated code refers to it) still reports its instruction count. The
// count must not be trusted as the bound: the array's own size is.
TEST(IrSeamA, ReleasedDecodedArrayDeclinesInsteadOfReadingOffTheEnd) {
    GuestMemory memory;
    JitCompiler jit(memory);

    const u8 code[] = {0x01, 0xC8, 0x31, 0xD2};
    BasicBlock block = decode_bytes(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 2u);

    block.instructions.reset();  // count stays 2, array is gone
    ASSERT_EQ(block.instruction_count, 2u);

    const ir::BuildResult& r = jit.ir_build_block(block);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, ir::DeclineReason::NoInstructions);
}

// A count larger than the array is the same hazard without the reset.
TEST(IrSeamA, CountExceedingTheArrayDeclines) {
    GuestMemory memory;
    JitCompiler jit(memory);

    const u8 code[] = {0x01, 0xC8};
    BasicBlock block = decode_bytes(code, sizeof(code));
    block.instruction_count = 99;

    const ir::BuildResult& r = jit.ir_build_block(block);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, ir::DeclineReason::NoInstructions);
}

// A realistic block — straight-line work ending in a conditional branch — is
// now ACCEPTED. This is the shape that kept the accept rate at 0% through
// M2-M4, because almost every real guest block ends in a branch.
TEST(IrSeamA, RealisticBlockEndingInABranchIsAccepted) {
    GuestMemory memory;
    JitCompiler jit(memory);

    const u8 code[] = {
        0x01, 0xC8,        // add eax, ecx
        0x74, 0x05,        // jz +5
    };
    BasicBlock block = decode_bytes(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 2u);

    const ir::BuildResult& r = jit.ir_build_block(block);
    EXPECT_TRUE(r.ok) << ir::decline_reason_name(r.reason);
}

// A block ending in RET is accepted: the return target is loaded from the
// stack and becomes an indirect terminator.
TEST(IrSeamA, BlockEndingInReturnIsAccepted) {
    GuestMemory memory;
    JitCompiler jit(memory);
    const u8 code[] = {0x01, 0xC8, 0xC3};
    BasicBlock block = decode_bytes(code, sizeof(code));
    const ir::BuildResult& r = jit.ir_build_block(block);
    EXPECT_TRUE(r.ok) << ir::decline_reason_name(r.reason);
}

// A block ending in an indirect CALL through memory — the import-thunk shape
// that dominates real 32-bit code — is accepted.
TEST(IrSeamA, BlockEndingInIndirectCallThroughMemoryIsAccepted) {
    GuestMemory memory;
    JitCompiler jit(memory);
    const u8 code[] = {0x01, 0xC8, 0xFF, 0x10};  // add eax,ecx ; call [eax]
    BasicBlock block = decode_bytes(code, sizeof(code));
    const ir::BuildResult& r = jit.ir_build_block(block);
    EXPECT_TRUE(r.ok) << ir::decline_reason_name(r.reason);
}

// Seam A must be invisible with the gate off, which is the default: a full
// `compile()` of a block the frontend would ACCEPT must still produce Tier 0
// code, byte-for-byte identical to a compile of the same block before the
// attempt existed. The strongest available check is that compilation still
// succeeds and emits a non-empty body — Tier 1 emits nothing in this milestone,
// so any change in outcome would mean the attempt perturbed emit state.
TEST(IrSeamA, AttemptDoesNotPerturbTierZeroCompilation) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                  MemoryProtection::ReadWrite));
    const u8 code[] = {
        0x01, 0xC8,                          // add eax, ecx
        0x31, 0xD2,                          // xor edx, edx
        0xB8, 0x11, 0x00, 0x00, 0x00,        // mov eax, 0x11
        0xC3,                                // ret
    };
    ASSERT_TRUE(memory.write_bytes(0x1000, code, sizeof(code)));

    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code, sizeof(code));
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr) << "Tier 0 must still compile this block";
    EXPECT_GT(compiled->code_size, 0u);
}
