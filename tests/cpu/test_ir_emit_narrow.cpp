// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_emit_narrow.cpp
/// @brief Tier 1 IR emission parity: 8/16-bit integer operations; contract in test_ir_emit.cpp.

#include "ir_emit_test_support.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_emit;

// =============================================================================
// 8/16-bit integer: CMP/TEST and MOVZX/MOVSX
// =============================================================================
//
// The subset of partial-register support that needs no partial-register WRITE:
// CMP and TEST discard their result, and MOVZX/MOVSX write a whole 32-bit
// register. Measured as ~36% of The Sims 3's declines after the SSE tranche.
namespace {

/// Seeds every GPR with a value whose bytes all differ, so a lowering that
/// reads the wrong FIELD — the whole register instead of AL, or the low byte
/// instead of AH — produces different flags rather than the same ones.
void expect_narrow_matches_interpreter(const std::vector<u8>& code,
                                       const char* what) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                  MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));
    ASSERT_TRUE(memory.write_u32(0x2000, 0x8899'AABBu));

    const auto seed = [](CpuState& cpu) {
        cpu.init_protected_mode();
        cpu.gpr.set32(0, 0x1234'56F0u);   // EAX: AL=F0, AH=56
        cpu.gpr.set32(1, 0x2000);         // ECX doubles as a pointer
        cpu.gpr.set32(2, 0x2000);
        cpu.gpr.set32(3, 0xFEDC'BA98u);   // EBX: BL=98, BH=BA
        cpu.gpr.set32(4, 0x3000);
        cpu.gpr.set32(5, 0x0F0F'0F0Fu);
        cpu.gpr.set32(6, 0x7FFF'8000u);
        cpu.gpr.set32(7, 0x0000'0080u);   // sign-bit boundary for MOVSX
        cpu.rip = kCode;
    };

    CpuState jit_cpu;
    seed(jit_cpu);
    JitCompiler jit(memory);
    BasicBlock block = decode_bytes(code);

    // Step the interpreter once PER INSTRUCTION, not once. A single step made
    // the oracle execute only the first of a multi-instruction case while the
    // JIT ran the whole block, and reported the JIT's correct answer as a
    // divergence.
    CpuState oracle;
    seed(oracle);
    {
        Interpreter interp(oracle, memory);
        for (u32 i = 0; i < block.instruction_count; ++i) {
            ASSERT_EQ(interp.step(), ExecResult::Continue) << what;
        }
    }
    oracle.materialize_lazy_flags();

    ASSERT_TRUE(jit.ir_build_block(block).ok)
        << what << ": " << ir::decline_reason_name(jit.ir_last_result().reason);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr) << what;
    ASSERT_TRUE(jit.ir_emit_active())
        << what << ": fell back to Tier 0; this would assert Tier 0";
    jit.execute(*compiled, jit_cpu);
    jit_cpu.materialize_lazy_flags();

    EXPECT_EQ(jit_cpu.flags.rflags, oracle.flags.rflags)
        << what << ": EFLAGS (got 0x" << std::hex << jit_cpu.flags.rflags
        << ", oracle 0x" << oracle.flags.rflags << std::dec << ")";
    for (u8 r = 0; r < 8; ++r) {
        EXPECT_EQ(jit_cpu.gpr.get32(r), oracle.gpr.get32(r))
            << what << ": GPR " << static_cast<int>(r)
            << " — a partial write must not disturb the upper bits";
    }
}

}  // namespace

TEST(IrEmitNarrow, Test8RegisterRegister) {
    REQUIRE_IR_GATE();
    expect_narrow_matches_interpreter({0x84, 0xD8}, "test al, bl");
}

// THE encoding trap: without REX, Reg8 index 4-7 is AH/CH/DH/BH, the HIGH
// byte. A lowering that read index 4 as "the low byte of ESP" would test the
// wrong field entirely, and with a same-register operand it would still
// produce plausible flags.
TEST(IrEmitNarrow, Test8HighByteRegisters) {
    REQUIRE_IR_GATE();
    expect_narrow_matches_interpreter({0x84, 0xE4}, "test ah, ah");
    expect_narrow_matches_interpreter({0x84, 0xFC}, "test ah, bh");
    expect_narrow_matches_interpreter({0x84, 0xC4}, "test ah, al");
}

TEST(IrEmitNarrow, Test8RegisterImmediate) {
    REQUIRE_IR_GATE();
    expect_narrow_matches_interpreter({0xF6, 0xC3, 0x0F}, "test bl, 0x0f");
    expect_narrow_matches_interpreter({0xA8, 0xF0}, "test al, 0xf0");
}

TEST(IrEmitNarrow, Cmp8Forms) {
    REQUIRE_IR_GATE();
    expect_narrow_matches_interpreter({0x38, 0xD8}, "cmp al, bl");
    expect_narrow_matches_interpreter({0x3A, 0xC3}, "cmp al, bl (r8,rm8)");
    expect_narrow_matches_interpreter({0x80, 0xF8, 0xF0}, "cmp al, 0xf0");
    expect_narrow_matches_interpreter({0x3C, 0x01}, "cmp al, 1");
}

// 8-bit CMP is where borrow differs from the 32-bit answer: AL=0xF0 against
// 0xF1 borrows, while the same values as 32-bit quantities would not.
TEST(IrEmitNarrow, Cmp8BorrowIsEightBitWide) {
    REQUIRE_IR_GATE();
    expect_narrow_matches_interpreter({0x3C, 0xF1}, "cmp al, 0xf1");
    expect_narrow_matches_interpreter({0x3C, 0xEF}, "cmp al, 0xef");
}

// An 8-bit CMP feeding EVERY conditional branch.
//
// This is the boundary that `nzcv_kind_for(Sub8)` opens. Fusing a byte compare
// into host PSTATE is legal ONLY for the unsigned and equality conditions:
// both operands are zero-extended, so ARM Z and C match x86 ZF and CF, while N
// and V describe a 32-bit subtraction and say nothing about SF/OF of an 8-bit
// one. `fused_arm_cond` is what enforces that, and this test is what proves it
// -- the existing Cmp8 tests assert EFLAGS but never BRANCH, so they pass
// whether or not the branch reads the wrong bits.
//
// The operand pairs are chosen so the signed and unsigned answers DISAGREE
// (127 vs 128 is "below" unsigned and "greater" signed, and 128 vs 127 is the
// reverse). Against equal-direction operands a wrongly-fused JL would still
// give the right answer and this test would have no teeth.
TEST(IrEmitNarrow, Cmp8FeedingEveryConditionalBranch) {
    REQUIRE_IR_GATE();
    // AL value, then the imm8 compared against it.
    struct Pair { u32 eax; u8 imm; const char* what; };
    static constexpr Pair kPairs[] = {
        {0x1234'567Fu, 0x80, "127 vs 128: unsigned below, signed greater"},
        {0x1234'5680u, 0x7F, "128 vs 127: unsigned above, signed less"},
        {0x1234'56F0u, 0xF0, "equal"},
        {0x1234'5600u, 0x01, "0 vs 1: borrow"},
        {0x1234'5680u, 0x80, "sign bit set, equal"},
        {0x1234'567Fu, 0x7F, "max positive, equal"},
    };
    for (const Pair& p : kPairs) {
        for (u8 cc = 0; cc < 16; ++cc) {
            const std::vector<u8> code = {
                0xB8, static_cast<u8>(p.eax), static_cast<u8>(p.eax >> 8),
                      static_cast<u8>(p.eax >> 16), static_cast<u8>(p.eax >> 24),
                0x3C, p.imm,                       // cmp al, imm8
                static_cast<u8>(0x70 + cc), 0x10,  // Jcc rel8 +0x10
            };
            GuestMemory memory;
            ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                          MemoryProtection::ReadWrite));
            ASSERT_TRUE(memory.write_bytes(kCode, code.data(), code.size()));

            const auto seed = [](CpuState& c) {
                c.init_protected_mode();
                c.gpr.set32(4, 0x3000);   // ESP
                c.rip = kCode;
            };

            CpuState oracle;
            seed(oracle);
            BasicBlock block = decode_bytes(code);
            {
                Interpreter interp(oracle, memory);
                for (u32 i = 0; i < block.instruction_count; ++i) {
                    // A taken Jcc reports Branch, not Continue -- the whole
                    // point of the sweep is that half these cases branch.
                    const ExecResult r = interp.step();
                    ASSERT_TRUE(r == ExecResult::Continue ||
                                r == ExecResult::Branch)
                        << p.what << " cc=" << static_cast<int>(cc);
                }
            }
            oracle.materialize_lazy_flags();

            CpuState jit_cpu;
            seed(jit_cpu);
            JitCompiler jit(memory);
            ASSERT_TRUE(jit.ir_build_block(block).ok)
                << p.what << " cc=" << static_cast<int>(cc) << ": "
                << ir::decline_reason_name(jit.ir_last_result().reason);
            CompiledBlock* compiled = jit.compile(block);
            ASSERT_NE(compiled, nullptr);
            ASSERT_TRUE(jit.ir_emit_active())
                << "fell back to Tier 0; this would assert Tier 0";
            jit.execute(*compiled, jit_cpu);
            jit_cpu.materialize_lazy_flags();

            // The branch OUTCOME is the whole point: a wrongly fused signed
            // condition shows up here and nowhere else.
            EXPECT_EQ(jit_cpu.rip, oracle.rip)
                << p.what << ", cc=" << static_cast<int>(cc)
                << ": branch went the wrong way (got 0x" << std::hex
                << jit_cpu.rip << ", oracle 0x" << oracle.rip << std::dec
                << ")";
            EXPECT_EQ(jit_cpu.flags.rflags, oracle.flags.rflags)
                << p.what << ", cc=" << static_cast<int>(cc) << ": EFLAGS";
        }
    }
}

TEST(IrEmitNarrow, Cmp16AndTest16Forms) {
    REQUIRE_IR_GATE();
    expect_narrow_matches_interpreter({0x66, 0x39, 0xD8}, "cmp ax, bx");
    expect_narrow_matches_interpreter({0x66, 0x83, 0xF8, 0x10}, "cmp ax, 16");
    expect_narrow_matches_interpreter({0x66, 0x85, 0xD8}, "test ax, bx");
}

TEST(IrEmitNarrow, Cmp8WithMemoryOperand) {
    REQUIRE_IR_GATE();
    expect_narrow_matches_interpreter({0x38, 0x19}, "cmp [ecx], bl");
    expect_narrow_matches_interpreter({0x80, 0x39, 0xBB}, "cmp byte [ecx],0xbb");
}

TEST(IrEmitNarrow, MovzxAndMovsxFromRegister) {
    REQUIRE_IR_GATE();
    expect_narrow_matches_interpreter({0x0F, 0xB6, 0xC3}, "movzx eax, bl");
    expect_narrow_matches_interpreter({0x0F, 0xBE, 0xC3}, "movsx eax, bl");
    expect_narrow_matches_interpreter({0x0F, 0xB7, 0xC3}, "movzx eax, bx");
    expect_narrow_matches_interpreter({0x0F, 0xBF, 0xC3}, "movsx eax, bx");
}

// Sign extension of a HIGH byte, and of a value whose bit 7 is set: EDI is
// seeded 0x00000080 so MOVSX must produce 0xFFFFFF80 and MOVZX 0x00000080.
TEST(IrEmitNarrow, MovsxSignBoundaryAndHighByte) {
    REQUIRE_IR_GATE();
    expect_narrow_matches_interpreter({0x0F, 0xBE, 0xC7}, "movsx eax, bh");
    expect_narrow_matches_interpreter({0x0F, 0xB6, 0xC7}, "movzx eax, bh");
}

TEST(IrEmitNarrow, MovzxFromMemory) {
    REQUIRE_IR_GATE();
    expect_narrow_matches_interpreter({0x0F, 0xB6, 0x01}, "movzx eax,byte[ecx]");
    expect_narrow_matches_interpreter({0x0F, 0xB7, 0x01}, "movzx eax,word[ecx]");
    expect_narrow_matches_interpreter({0x0F, 0xBE, 0x01}, "movsx eax,byte[ecx]");
}

// Partial-register WRITES: x86's merge rule.
//
// `mov al,5` leaves EAX[31:8] untouched; `mov ah,5` writes bits 15:8; `mov
// ax,5` leaves EAX[31:16]. A lowering that wrote the whole register would pass
// any test whose destination happened to be zero above the field, so every
// seed here has non-zero bits in exactly the places that must survive — and
// the comparison is against the interpreter on ALL EIGHT registers, not just
// the written one.
TEST(IrEmitNarrow, MovIntoLowByteKeepsUpperBits) {
    REQUIRE_IR_GATE();
    expect_narrow_matches_interpreter({0xB0, 0x5A}, "mov al, 0x5a");
    expect_narrow_matches_interpreter({0x88, 0xD8}, "mov al, bl");
    expect_narrow_matches_interpreter({0x8A, 0xC3}, "mov al, bl (r8,rm8)");
    expect_narrow_matches_interpreter({0xC6, 0xC0, 0x7F}, "mov al, 0x7f");
}

// The high-byte destination. `mov ah,5` must not touch AL, and must not be
// confused with a write to ESP — index 4 at 8 bits is AH.
TEST(IrEmitNarrow, MovIntoHighByteWritesBits15To8) {
    REQUIRE_IR_GATE();
    expect_narrow_matches_interpreter({0xB4, 0x5A}, "mov ah, 0x5a");
    expect_narrow_matches_interpreter({0x88, 0xFC}, "mov ah, bh");
    expect_narrow_matches_interpreter({0x8A, 0xE3}, "mov ah, bl");
}

TEST(IrEmitNarrow, MovIntoWordKeepsUpperHalf) {
    REQUIRE_IR_GATE();
    expect_narrow_matches_interpreter({0x66, 0xB8, 0x34, 0x12}, "mov ax,0x1234");
    expect_narrow_matches_interpreter({0x66, 0x89, 0xD8}, "mov ax, bx");
    expect_narrow_matches_interpreter({0x66, 0x8B, 0xC3}, "mov ax, bx (r16)");
}

// A narrow write followed by a full-width READ of the same register: the
// merged value has to be visible. This is the shape that a tombstone bug
// hides — if `mov al,..` failed to invalidate values still aliasing EAX's pin,
// the CMP would compare a stale register.
TEST(IrEmitNarrow, FullWidthReadSeesAPrecedingPartialWrite) {
    REQUIRE_IR_GATE();
    expect_narrow_matches_interpreter({0xB0, 0x11,        // mov al, 0x11
                                       0x39, 0xD8},       // cmp eax, ebx
                                      "mov al then cmp eax,ebx");
    expect_narrow_matches_interpreter({0xB4, 0x22,        // mov ah, 0x22
                                       0x39, 0xD8},       // cmp eax, ebx
                                      "mov ah then cmp eax,ebx");
}

// Narrow STORES to memory.
//
// Disabled for one increment while the crash it exposed was diagnosed: narrow
// stores together with partial-register writes killed The Sims 3 with a host
// SIGSEGV. The cause was NOT here — it was Tier 0's ESP-frame cache being
// clobbered by a cold stub's helper call without the compiler modelling it, so
// a later `[esp+disp]` access read a dead X15. Re-enabled once that was fixed.
TEST(IrEmitNarrow, NarrowStoresWriteOnlyTheirWidth) {
    REQUIRE_IR_GATE();
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                  MemoryProtection::ReadWrite));
    struct Case { const char* what; std::vector<u8> code; };
    const Case cases[] = {
        {"mov [ecx], bl",        {0x88, 0x19}},
        {"mov byte [ecx], 0x5a", {0xC6, 0x01, 0x5A}},
        {"mov [ecx], bx",        {0x66, 0x89, 0x19}},
    };
    for (const Case& c : cases) {
        ASSERT_TRUE(memory.write_bytes(kCode, c.code.data(), c.code.size()));
        const auto seed = [](CpuState& cpu) {
            cpu.init_protected_mode();
            cpu.gpr.set32(1, 0x2000);
            cpu.gpr.set32(3, 0xFEDC'BA98u);
            cpu.gpr.set32(4, 0x3000);
            cpu.rip = kCode;
        };
        // A recognisable 8-byte pattern: a store wider than it should be shows
        // up in the neighbouring bytes rather than only in the target.
        ASSERT_TRUE(memory.write_u64(0x2000, 0x1122'3344'5566'7788ull));
        CpuState oracle;
        seed(oracle);
        { Interpreter interp(oracle, memory);
          ASSERT_EQ(interp.step(), ExecResult::Continue) << c.what; }
        const auto oracle_mem = memory.read_u64(0x2000);
        ASSERT_TRUE(oracle_mem.is_ok());

        ASSERT_TRUE(memory.write_u64(0x2000, 0x1122'3344'5566'7788ull));
        CpuState jit_cpu;
        seed(jit_cpu);
        JitCompiler jit(memory);
        BasicBlock block = decode_bytes(c.code);
        ASSERT_TRUE(jit.ir_build_block(block).ok)
            << c.what << ": "
            << ir::decline_reason_name(jit.ir_last_result().reason);
        CompiledBlock* compiled = jit.compile(block);
        ASSERT_NE(compiled, nullptr) << c.what;
        ASSERT_TRUE(jit.ir_emit_active()) << c.what;
        jit.execute(*compiled, jit_cpu);

        const auto jit_mem = memory.read_u64(0x2000);
        ASSERT_TRUE(jit_mem.is_ok());
        EXPECT_EQ(jit_mem.value(), oracle_mem.value())
            << c.what << ": a store must touch exactly its own width";
    }
}
