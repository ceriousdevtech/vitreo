// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): LOCK-prefixed forms
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

// A pending lazy flag op must not survive LOCK CMPXCHG into the Jcc that
// follows it: CMPXCHG rewrites all six status flags from (dest - accumulator),
// so the Jcc must see ITS result, never the CMP's.
//
// This is the shape of a guest lock-acquire loop (`lock cmpxchg` + retry), so
// the failure mode is not a crash: the loop spins forever. The operands are
// chosen so the two ZF values disagree -- the CMP compares equal (ZF=1) and the
// CMPXCHG compares unequal (ZF=0).
//
// Honest scope: this also passed with the helper's former opt-in discard path
// (removed 2026-10-11), so a carried ARM NZCV is evidently not reachable
// through the current CMPXCHG lowering. It is a parity guard on the flag
// result itself.
TEST_F(JitParityTest, LockCmpxchgClearsCarriedNzcvBeforeJcc) {
    JitCompiler jit(*m_memory);
    // cmp eax, ecx ; lock cmpxchg [ebx], edx ; jz +2
    const u8 code[] = {0x39, 0xC8, 0xF0, 0x0F, 0xB1, 0x13, 0x74, 0x02};
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    MemoryByteReader reader(code, sizeof(code));
    BasicBlock block;
    block.start_addr = 0x1000;
    while (reader.remaining() > 0) {
        DecodedInstruction inst;
        if (decode_instruction(reader, DecodeMode::Mode32, inst) !=
            DecodeError::Success)
            break;
        block.instructions.push_back(inst);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    ASSERT_EQ(block.instruction_count, 3u);

    constexpr GuestAddr kCell = 0x2000;
    auto setup = [](CpuState& c) {
        c.init_protected_mode();
        c.gpr.set32(ESP, 0x10000);
        c.rip = 0x1000;
        c.gpr.set32(EAX, 5);        // CMP compares equal -> ZF=1
        c.gpr.set32(ECX, 5);
        c.gpr.set32(EDX, 0x1234);   // value CMPXCHG would store
        c.gpr.set32(EBX, kCell);
    };

    CpuState icpu;
    setup(icpu);
    ASSERT_TRUE(m_memory->write_u32(kCell, 7).is_ok());  // != EAX -> ZF=0
    {
        Interpreter interp(icpu, *m_memory);
        (void)interp.run(3);
    }
    const u32 interp_cell = m_memory->read_u32(kCell).value_or(0);

    CpuState jcpu;
    setup(jcpu);
    ASSERT_TRUE(m_memory->write_u32(kCell, 7).is_ok());
    ASSERT_TRUE(m_memory
                    ->protect_region(0x1000, 0x1000,
                                     MemoryProtection::ReadExecute)
                    .is_ok());
    CompiledBlock* cb = jit.compile(block);
    ASSERT_NE(cb, nullptr);
    (void)test::execute_like_dispatcher(jit, *cb, jcpu);
    ASSERT_TRUE(m_memory
                    ->protect_region(0x1000, 0x1000,
                                     MemoryProtection::ReadWrite)
                    .is_ok());

    icpu.materialize_lazy_flags();
    jcpu.materialize_lazy_flags();

    EXPECT_FALSE(jcpu.flags.zf())
        << "CMPXCHG compared unequal; a stale ZF from the CMP spins the "
           "acquire loop forever";
    EXPECT_EQ(icpu.flags.zf(), jcpu.flags.zf());
    EXPECT_EQ(static_cast<u32>(icpu.rip), static_cast<u32>(jcpu.rip))
        << "JZ took a different edge than the interpreter";
    // CMPXCHG loads the destination into EAX when the comparison fails.
    EXPECT_EQ(jcpu.gpr.get32(EAX), 7u);
    EXPECT_EQ(icpu.gpr.get32(EAX), jcpu.gpr.get32(EAX));
    EXPECT_EQ(m_memory->read_u32(kCell).value_or(0), interp_cell);
}

// `LOCK XCHG [mem], r32` must lower natively AND leave EFLAGS alone.
//
// Two independent regressions are guarded here. (1) `compile_locked` had no
// XCHG r/m32,r32 arm, so every execution took the per-instruction interpreter
// delegate — whose dispatcher-level LockPrefixGuard has no operand information
// and therefore serialises the whole process. (2) The lowering reaches its
// helper through a BLR, which destroys host NZCV, while Intel SDM Vol. 2 XCHG
// modifies no flags: a CMP before it must still decide the Jcc after it.
TEST_F(JitParityTest, LockXchg32IsNativeAndPreservesFlags) {
    JitCompiler jit(*m_memory);
    // cmp eax, ecx ; lock xchg [ebx], edx ; jz +2
    const u8 code[] = {0x39, 0xC8, 0xF0, 0x87, 0x13, 0x74, 0x02};
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    MemoryByteReader reader(code, sizeof(code));
    BasicBlock block;
    block.start_addr = 0x1000;
    while (reader.remaining() > 0) {
        DecodedInstruction inst;
        if (decode_instruction(reader, DecodeMode::Mode32, inst) !=
            DecodeError::Success)
            break;
        block.instructions.push_back(inst);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    ASSERT_EQ(block.instruction_count, 3u);

    constexpr GuestAddr kCell = 0x2000;
    constexpr u32 kHeld = 0x11111111u;   // what the cell holds
    constexpr u32 kWanted = 0x22222222u; // what EDX swaps in
    auto setup = [](CpuState& c) {
        c.init_protected_mode();
        c.gpr.set32(ESP, 0x10000);
        c.rip = 0x1000;
        c.gpr.set32(EAX, 5);   // CMP unequal -> ZF=0, so JZ must NOT be taken
        c.gpr.set32(ECX, 3);
        c.gpr.set32(EDX, kWanted);
        c.gpr.set32(EBX, kCell);
    };

    CpuState icpu;
    setup(icpu);
    ASSERT_TRUE(m_memory->write_u32(kCell, kHeld).is_ok());
    {
        Interpreter interp(icpu, *m_memory);
        (void)interp.run(3);
    }
    const u32 interp_cell = m_memory->read_u32(kCell).value_or(0);

    CpuState jcpu;
    setup(jcpu);
    ASSERT_TRUE(m_memory->write_u32(kCell, kHeld).is_ok());
    ASSERT_TRUE(m_memory
                    ->protect_region(0x1000, 0x1000,
                                     MemoryProtection::ReadExecute)
                    .is_ok());
    CompiledBlock* cb = jit.compile(block);
    ASSERT_NE(cb, nullptr);
    // The whole point: no instruction in this block may delegate.
    EXPECT_EQ(cb->native_insns, block.instruction_count)
        << "LOCK XCHG r/m32,r32 delegated to the interpreter; its guard then "
           "takes the process-wide exclusive lock";
    (void)test::execute_like_dispatcher(jit, *cb, jcpu);
    ASSERT_TRUE(m_memory
                    ->protect_region(0x1000, 0x1000,
                                     MemoryProtection::ReadWrite)
                    .is_ok());

    icpu.materialize_lazy_flags();
    jcpu.materialize_lazy_flags();

    // The exchange itself.
    EXPECT_EQ(m_memory->read_u32(kCell).value_or(0), kWanted);
    EXPECT_EQ(m_memory->read_u32(kCell).value_or(0), interp_cell);
    EXPECT_EQ(jcpu.gpr.get32(EDX), kHeld) << "EDX must receive the old cell";
    EXPECT_EQ(icpu.gpr.get32(EDX), jcpu.gpr.get32(EDX));
    // The CMP's flags must survive the helper call, and the JZ must agree.
    EXPECT_FALSE(jcpu.flags.zf()) << "XCHG modifies no flags (SDM Vol. 2)";
    EXPECT_EQ(icpu.flags.zf(), jcpu.flags.zf());
    EXPECT_EQ(static_cast<u32>(icpu.rip), static_cast<u32>(jcpu.rip))
        << "JZ took a different edge than the interpreter";
}

// ---------------------------------------------------------------------------
// LOCK r/m32 forms that had no arm in compile_locked and therefore reached the
// per-instruction interpreter delegate. That delegate's LockPrefixGuard is
// built with NO operand information, so it takes the PROCESS-WIDE exclusive
// mutex and waits out every other thread's reader slot — the same defect
// XCHG_RM32_R32 produced 7.48M times a minute in one renderer.
//
// Each test asserts BOTH halves: `native_insns == instruction_count` (nothing
// delegated) and interpreter parity (the lowering is right). Deleting the
// corresponding arm from compile_locked must fail the native_insns assertion.
// ---------------------------------------------------------------------------

namespace {

/// Decode `code` as one 32-bit block at `at`.
BasicBlock decode32_block(GuestMemory& memory, const u8* code, usize len,
                          GuestAddr at) {
    EXPECT_TRUE(memory.write_bytes(at, code, len));
    MemoryByteReader reader(code, len);
    BasicBlock block;
    block.start_addr = at;
    while (reader.remaining() > 0) {
        DecodedInstruction inst;
        if (decode_instruction(reader, DecodeMode::Mode32, inst) !=
            DecodeError::Success)
            break;
        block.instructions.push_back(inst);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    return block;
}

}  // namespace

// `lock bts dword ptr [ebx], ecx` with a NEGATIVE bit index.
//
// This is the sharp edge of the change. Intel SDM Vol. 2 BTS: with a memory bit
// base and a REGISTER offset, the offset is signed and selects a bit in a bit
// string — byte = base + (index >> 3), bit = index & 7 — so a negative index
// must touch a byte BELOW the operand. `exec_bt` sign-extends at the index
// register's own width, i32 here.
//
// ECX = -8 must therefore set bit 0 of the byte at kCell-1. Zero-extending
// instead gives byte_off = 0x1FFFFFFF and writes ~512 MiB above the cell — a
// silently corrupted bitmap, or an access violation.
TEST_F(JitParityTest, LockBts32NegativeIndexIsNativeAndSignExtends) {
    JitCompiler jit(*m_memory);
    const u8 code[] = {0xF0, 0x0F, 0xAB, 0x0B};  // lock bts [ebx], ecx
    BasicBlock block = decode32_block(*m_memory, code, sizeof(code), 0x1000);
    ASSERT_EQ(block.instruction_count, 1u);

    // kCell is 0x3000, not 0x2000, precisely because a negative index writes
    // BELOW it: kCell-1 must stay writable while [0x1000,0x2000) is flipped to
    // ReadExecute for the JIT run. The fixture maps [0x1000,0x11000) RWX.
    constexpr GuestAddr kCell = 0x3000;
    constexpr GuestAddr kBelow = kCell - 1;  // the byte a -8 index selects
    auto setup = [](CpuState& c) {
        c.init_protected_mode();
        c.gpr.set32(ESP, 0x10000);
        c.rip = 0x1000;
        c.gpr.set32(EBX, kCell);
        c.gpr.set32(ECX, static_cast<u32>(-8));  // byte_off -1, bit 0
    };

    CpuState icpu;
    setup(icpu);
    ASSERT_TRUE(m_memory->write_u8(kBelow, 0x00).is_ok());
    {
        Interpreter interp(icpu, *m_memory);
        (void)interp.run(1);
    }
    const u8 interp_byte = m_memory->read_u8(kBelow).value_or(0xFF);
    // Guard the premise: if the interpreter did not do this, comparing the JIT
    // against it below would be meaningless.
    ASSERT_EQ(interp_byte, 0x01u)
        << "interpreter did not set bit 0 of the byte below the operand";

    CpuState jcpu;
    setup(jcpu);
    ASSERT_TRUE(m_memory->write_u8(kBelow, 0x00).is_ok());
    ASSERT_TRUE(m_memory
                    ->protect_region(0x1000, 0x1000,
                                     MemoryProtection::ReadExecute)
                    .is_ok());
    CompiledBlock* cb = jit.compile(block);
    ASSERT_NE(cb, nullptr);
    EXPECT_EQ(cb->native_insns, block.instruction_count)
        << "LOCK BTS r/m32,r32 delegated to the interpreter; its guard then "
           "takes the process-wide exclusive lock";
    (void)test::execute_like_dispatcher(jit, *cb, jcpu);
    ASSERT_TRUE(m_memory
                    ->protect_region(0x1000, 0x1000,
                                     MemoryProtection::ReadWrite)
                    .is_ok());

    icpu.materialize_lazy_flags();
    jcpu.materialize_lazy_flags();
    EXPECT_EQ(m_memory->read_u8(kBelow).value_or(0xFF), interp_byte)
        << "sign-extension of the bit index disagrees with the interpreter";
    EXPECT_EQ(icpu.flags.cf(), jcpu.flags.cf())
        << "CF must be the PREVIOUS value of the selected bit";
    EXPECT_FALSE(jcpu.flags.cf()) << "the bit was clear before the BTS";
}

// `lock xor dword ptr [ebx], edx` — OR/AND/ADD/SUB r/m32 were wired and XOR was
// not, so this is a pure wiring gap. Intel SDM Vol. 2 XOR: ZF/SF/PF from the
// result, CF and OF cleared.
TEST_F(JitParityTest, LockXor32IsNativeAndMatchesInterpreter) {
    JitCompiler jit(*m_memory);
    const u8 code[] = {0xF0, 0x31, 0x13};  // lock xor [ebx], edx
    BasicBlock block = decode32_block(*m_memory, code, sizeof(code), 0x1000);
    ASSERT_EQ(block.instruction_count, 1u);

    constexpr GuestAddr kCell = 0x2000;
    constexpr u32 kHeld = 0xF0F0F0F0u;
    constexpr u32 kMask = 0x0FF00FF0u;
    auto setup = [](CpuState& c) {
        c.init_protected_mode();
        c.gpr.set32(ESP, 0x10000);
        c.rip = 0x1000;
        c.gpr.set32(EBX, kCell);
        c.gpr.set32(EDX, kMask);
    };

    CpuState icpu;
    setup(icpu);
    ASSERT_TRUE(m_memory->write_u32(kCell, kHeld).is_ok());
    {
        Interpreter interp(icpu, *m_memory);
        (void)interp.run(1);
    }
    const u32 interp_cell = m_memory->read_u32(kCell).value_or(0);

    CpuState jcpu;
    setup(jcpu);
    ASSERT_TRUE(m_memory->write_u32(kCell, kHeld).is_ok());
    ASSERT_TRUE(m_memory
                    ->protect_region(0x1000, 0x1000,
                                     MemoryProtection::ReadExecute)
                    .is_ok());
    CompiledBlock* cb = jit.compile(block);
    ASSERT_NE(cb, nullptr);
    EXPECT_EQ(cb->native_insns, block.instruction_count)
        << "LOCK XOR r/m32 delegated to the interpreter";
    (void)test::execute_like_dispatcher(jit, *cb, jcpu);
    ASSERT_TRUE(m_memory
                    ->protect_region(0x1000, 0x1000,
                                     MemoryProtection::ReadWrite)
                    .is_ok());

    icpu.materialize_lazy_flags();
    jcpu.materialize_lazy_flags();
    EXPECT_EQ(m_memory->read_u32(kCell).value_or(0), kHeld ^ kMask);
    EXPECT_EQ(m_memory->read_u32(kCell).value_or(0), interp_cell);
    EXPECT_EQ(icpu.flags.zf(), jcpu.flags.zf());
    EXPECT_EQ(icpu.flags.sf(), jcpu.flags.sf());
    EXPECT_EQ(icpu.flags.pf(), jcpu.flags.pf());
    EXPECT_FALSE(jcpu.flags.cf()) << "XOR clears CF";
    EXPECT_FALSE(jcpu.flags.of()) << "XOR clears OF";
}

// `lock neg dword ptr [ebx]` — the width-sensitive one. update_flags_sub is a
// template, so a 32-bit NEG whose operands were widened to u64 would take SF
// and OF from bit 63 of a zero-extended value. 0x80000000 separates them: NEG
// of INT32_MIN sets OF at 32 bits and does not at 64.
TEST_F(JitParityTest, LockNeg32IsNativeAndUsesThirtyTwoBitFlagWidths) {
    constexpr GuestAddr kCell = 0x2000;
    for (const u32 held : {0x00000001u, 0x00000000u, 0x80000000u,
                           0xFFFFFFFFu}) {
        JitCompiler jit(*m_memory);
        const u8 code[] = {0xF0, 0xF7, 0x1B};  // lock neg dword [ebx]
        BasicBlock block =
            decode32_block(*m_memory, code, sizeof(code), 0x1000);
        ASSERT_EQ(block.instruction_count, 1u);

        auto setup = [](CpuState& c) {
            c.init_protected_mode();
            c.gpr.set32(ESP, 0x10000);
            c.rip = 0x1000;
            c.gpr.set32(EBX, kCell);
        };

        CpuState icpu;
        setup(icpu);
        ASSERT_TRUE(m_memory->write_u32(kCell, held).is_ok());
        {
            Interpreter interp(icpu, *m_memory);
            (void)interp.run(1);
        }
        const u32 interp_cell = m_memory->read_u32(kCell).value_or(0);

        CpuState jcpu;
        setup(jcpu);
        ASSERT_TRUE(m_memory->write_u32(kCell, held).is_ok());
        ASSERT_TRUE(m_memory
                        ->protect_region(0x1000, 0x1000,
                                         MemoryProtection::ReadExecute)
                        .is_ok());
        CompiledBlock* cb = jit.compile(block);
        ASSERT_NE(cb, nullptr);
        EXPECT_EQ(cb->native_insns, block.instruction_count)
            << "LOCK NEG r/m32 delegated (held=" << std::hex << held << ")";
        (void)test::execute_like_dispatcher(jit, *cb, jcpu);
        ASSERT_TRUE(m_memory
                        ->protect_region(0x1000, 0x1000,
                                         MemoryProtection::ReadWrite)
                        .is_ok());

        icpu.materialize_lazy_flags();
        jcpu.materialize_lazy_flags();
        EXPECT_EQ(m_memory->read_u32(kCell).value_or(0), 0u - held)
            << "held=" << std::hex << held;
        EXPECT_EQ(m_memory->read_u32(kCell).value_or(0), interp_cell);
        EXPECT_EQ(icpu.flags.cf(), jcpu.flags.cf()) << std::hex << held;
        EXPECT_EQ(icpu.flags.zf(), jcpu.flags.zf()) << std::hex << held;
        EXPECT_EQ(icpu.flags.sf(), jcpu.flags.sf()) << std::hex << held;
        EXPECT_EQ(icpu.flags.of(), jcpu.flags.of()) << std::hex << held;
        EXPECT_EQ(icpu.flags.af(), jcpu.flags.af()) << std::hex << held;
        EXPECT_EQ(jcpu.flags.cf(), held != 0u)
            << "NEG sets CF exactly when the source was nonzero";
    }
}

// `lock not dword ptr [ebx]` — Intel SDM Vol. 2 NOT affects NO flags, so a
// preceding CMP must survive the helper call intact.
TEST_F(JitParityTest, LockNot32IsNativeAndLeavesFlagsAlone) {
    JitCompiler jit(*m_memory);
    // cmp eax, ecx ; lock not dword [ebx]
    const u8 code[] = {0x39, 0xC8, 0xF0, 0xF7, 0x13};
    BasicBlock block = decode32_block(*m_memory, code, sizeof(code), 0x1000);
    ASSERT_EQ(block.instruction_count, 2u);

    constexpr GuestAddr kCell = 0x2000;
    constexpr u32 kHeld = 0x0F0F0F0Fu;
    auto setup = [](CpuState& c) {
        c.init_protected_mode();
        c.gpr.set32(ESP, 0x10000);
        c.rip = 0x1000;
        c.gpr.set32(EAX, 7);  // CMP equal -> ZF=1; NOT must not disturb it
        c.gpr.set32(ECX, 7);
        c.gpr.set32(EBX, kCell);
    };

    CpuState icpu;
    setup(icpu);
    ASSERT_TRUE(m_memory->write_u32(kCell, kHeld).is_ok());
    {
        Interpreter interp(icpu, *m_memory);
        (void)interp.run(2);
    }
    const u32 interp_cell = m_memory->read_u32(kCell).value_or(0);

    CpuState jcpu;
    setup(jcpu);
    ASSERT_TRUE(m_memory->write_u32(kCell, kHeld).is_ok());
    ASSERT_TRUE(m_memory
                    ->protect_region(0x1000, 0x1000,
                                     MemoryProtection::ReadExecute)
                    .is_ok());
    CompiledBlock* cb = jit.compile(block);
    ASSERT_NE(cb, nullptr);
    EXPECT_EQ(cb->native_insns, block.instruction_count)
        << "LOCK NOT r/m32 delegated to the interpreter";
    (void)test::execute_like_dispatcher(jit, *cb, jcpu);
    ASSERT_TRUE(m_memory
                    ->protect_region(0x1000, 0x1000,
                                     MemoryProtection::ReadWrite)
                    .is_ok());

    icpu.materialize_lazy_flags();
    jcpu.materialize_lazy_flags();
    EXPECT_EQ(m_memory->read_u32(kCell).value_or(0), ~kHeld);
    EXPECT_EQ(m_memory->read_u32(kCell).value_or(0), interp_cell);
    EXPECT_TRUE(jcpu.flags.zf()) << "NOT must not clear the CMP's ZF";
    EXPECT_EQ(icpu.flags.zf(), jcpu.flags.zf());
    EXPECT_EQ(icpu.flags.cf(), jcpu.flags.cf());
    EXPECT_EQ(icpu.flags.sf(), jcpu.flags.sf());
}
