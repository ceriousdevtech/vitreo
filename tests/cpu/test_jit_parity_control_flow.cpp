// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): LOOP/JCXZ, RET imm16 and indirect CALL
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

TEST_F(JitParityTest, LoopRel8FallsThroughWhenCountBecomesZero) {
    // LOOP +2 with ECX=1 → ECX=0, fall through (no branch).
    check_parity({0xE2, 0x02},
        [](CpuState& cpu) { cpu.gpr.set32(ECX, 1); },
        false);
}

TEST_F(JitParityTest, LoopRel8AddressSizeUsesCx) {
    // 67 E2 02 — AddressSize override: decrement CX only.
    // ECX=0x00010001 → CX=1 → after LOOP CX=0, ECX=0x00010000.
    check_parity({0x67, 0xE2, 0x02},
        [](CpuState& cpu) { cpu.gpr.set32(ECX, 0x00010001u); },
        false);
}

TEST_F(JitParityTest, JecxzFallsThroughWhenEcxNonzero) {
    check_parity({0xE3, 0x02},
        [](CpuState& cpu) { cpu.gpr.set32(ECX, 5); },
        false);
}

TEST_F(JitParityTest, JcxzAddressSizeUsesCx) {
    // 67 E3 02 — JCXZ: CX==0 but upper ECX bits set → taken? No: CX from
    // ECX=0x00010000 is 0 → branch taken. Use CX!=0 so fall-through for
    // check_parity (expects Continue).
    check_parity({0x67, 0xE3, 0x02},
        [](CpuState& cpu) { cpu.gpr.set32(ECX, 0x00010001u); },
        false);
}

TEST_F(JitParityTest, LoopeFallsThroughOnClearZf) {
    // LOOPE +2 with ECX=2, ZF=0 → decrement then no branch.
    check_parity({0xE1, 0x02},
        [](CpuState& cpu) {
            cpu.gpr.set32(ECX, 2);
            cpu.flags.set_zf(false);
        },
        false);
}

TEST_F(JitParityTest, LoopneFallsThroughOnSetZf) {
    // LOOPNE +2 with ECX=2, ZF=1 → decrement then no branch.
    check_parity({0xE0, 0x02},
        [](CpuState& cpu) {
            cpu.gpr.set32(ECX, 2);
            cpu.flags.set_zf(true);
        },
        false);
}

// Taken LOOP/JCXZ paths return Branch from the interpreter, so compare RIP/ECX
// directly (same pattern as expect_branch_matches for Jcc).
namespace {
void expect_loop_jcxz_matches(GuestMemory& mem,
                              std::initializer_list<u8> code_il,
                              std::function<void(CpuState&)> setup) {
    JitCompiler jit(mem);
    std::vector<u8> code(code_il);
    ASSERT_TRUE(mem.write_bytes(0x1000, code.data(), code.size()));
    MemoryByteReader reader(code.data(), code.size());
    BasicBlock block;
    block.start_addr = 0x1000;
    while (reader.remaining() > 0) {
        DecodedInstruction inst;
        if (decode_instruction(reader, DecodeMode::Mode32, inst) !=
            DecodeError::Success) {
            break;
        }
        block.instructions.push_back(inst);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    ASSERT_GE(block.instruction_count, 1u);

    auto init = [&](CpuState& c) {
        c.init_protected_mode();
        c.gpr.set32(ESP, 0x10000);
        c.rip = 0x1000;
        if (setup) setup(c);
    };
    CpuState icpu;
    init(icpu);
    {
        Interpreter interp(icpu, mem);
        (void)interp.run(block.instruction_count);
    }
    CpuState jcpu;
    init(jcpu);
    ASSERT_TRUE(mem.protect_region(0x1000, 0x1000,
                                   MemoryProtection::ReadExecute).is_ok());
    CompiledBlock* cb = jit.compile(block);
    ASSERT_NE(cb, nullptr);
    (void)test::execute_like_dispatcher(jit, *cb, jcpu);
    ASSERT_TRUE(mem.protect_region(0x1000, 0x1000,
                                   MemoryProtection::ReadWrite).is_ok());

    EXPECT_EQ(static_cast<u32>(icpu.rip), static_cast<u32>(jcpu.rip))
        << "EIP diverged";
    EXPECT_EQ(icpu.gpr.get32(ECX), jcpu.gpr.get32(ECX)) << "ECX diverged";
}
}  // namespace

TEST_F(JitParityTest, LoopRel8TakenMatchesInterpreter) {
    // LOOP +4 with ECX=3 → ECX=2, EIP=0x1006
    expect_loop_jcxz_matches(*m_memory, {0xE2, 0x04},
        [](CpuState& c) { c.gpr.set32(ECX, 3); });
}

TEST_F(JitParityTest, LoopeTakenWhenZfSet) {
    expect_loop_jcxz_matches(*m_memory, {0xE1, 0x04},
        [](CpuState& c) {
            c.gpr.set32(ECX, 3);
            c.flags.set_zf(true);
        });
}

TEST_F(JitParityTest, LoopneTakenWhenZfClear) {
    expect_loop_jcxz_matches(*m_memory, {0xE0, 0x04},
        [](CpuState& c) {
            c.gpr.set32(ECX, 3);
            c.flags.set_zf(false);
        });
}

TEST_F(JitParityTest, JecxzTakenWhenEcxZero) {
    expect_loop_jcxz_matches(*m_memory, {0xE3, 0x04},
        [](CpuState& c) { c.gpr.set32(ECX, 0); });
}

TEST_F(JitParityTest, JcxzTakenWhenCxZeroHighEcxNonzero) {
    // 67 E3 04 — CX==0 from ECX=0x00020000 → taken; ECX unchanged.
    expect_loop_jcxz_matches(*m_memory, {0x67, 0xE3, 0x04},
        [](CpuState& c) { c.gpr.set32(ECX, 0x00020000u); });
}

// ============================================================================
// RET imm16 (C2 iw) and its 66H form -- Intel SDM Vol. 2 RET, near return.
//
//   OperandSize = 32: EIP := Pop()                      (4 bytes)
//   OperandSize = 16: tempEIP := Pop(); EIP := tempEIP AND 0000FFFFH (2 bytes)
//   then, with an immediate and StackAddressSize = 32: ESP := ESP + SRC
//
// SRC is the UNSIGNED imm16, so ESP moves by slot + imm16, up to 65539 --
// wider than the 12-bit field of ARM64 ADD (immediate), which the lowering
// once encoded it in. A flat 32-bit SS makes the stack address size 32, so
// the sum wraps modulo 2^32. Flags Affected: None. Every expected value below
// is that pseudocode evaluated by hand, not either engine's output.
// ============================================================================
namespace {
struct RetRun {
    CpuState interp{};
    CpuState jit{};
    ExecResult interp_result{ExecResult::Continue};
    // The interpreter's exception record when it faulted.
    CpuException interp_exception{CpuException::None};
    u32 interp_exception_error{0};
    u64 interp_exception_addr{0};
};

constexpr u64 kRetFlags = 0x202u | 0x8D5u;   // CF PF AF ZF SF OF set

RetRun run_ret32_both(GuestMemory& mem, const std::vector<u8>& code,
                      u32 esp) {
    RetRun run;
    EXPECT_TRUE(mem.write_bytes(0x1000, code.data(), code.size()).is_ok());
    BasicBlock block;
    block.start_addr = 0x1000;
    MemoryByteReader reader(code.data(), code.size());
    while (reader.remaining() > 0) {
        DecodedInstruction inst;
        if (decode_instruction(reader, DecodeMode::Mode32, inst) !=
            DecodeError::Success) {
            break;
        }
        block.instructions.push_back(inst);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    EXPECT_EQ(block.instruction_count, 1u);

    const auto init = [&](CpuState& cpu) {
        cpu.init_protected_mode();
        cpu.gpr.set32(ESP, esp);
        cpu.rip = 0x1000;
        cpu.flags.rflags = kRetFlags;
    };
    init(run.interp);
    {
        Interpreter interp(run.interp, mem);
        run.interp_result = interp.step();
        run.interp_exception = interp.context().exception;
        run.interp_exception_error = interp.context().exception_error;
        run.interp_exception_addr = interp.context().exception_addr;
    }
    run.interp.materialize_lazy_flags();

    init(run.jit);
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    JitCompiler jit(mem);
    CompiledBlock* cb = jit.compile(block);
    EXPECT_NE(cb, nullptr) << "RET imm16 must lower natively";
    if (cb != nullptr) (void)test::execute_like_dispatcher(jit, *cb, run.jit);
    run.jit.materialize_lazy_flags();
    return run;
}

void expect_ret32_completed(const RetRun& run, u32 esp_after, u32 eip_after) {
    EXPECT_EQ(run.interp_result, ExecResult::Branch);
    EXPECT_FALSE(run.jit.pending_raise.active);
    for (const CpuState* cpu : {&run.interp, &run.jit}) {
        const char* engine = cpu == &run.jit ? "jit" : "interp";
        EXPECT_EQ(cpu->gpr.get32(ESP), esp_after) << engine << " ESP";
        EXPECT_EQ(cpu->rip, u64{eip_after}) << engine << " EIP";
        EXPECT_EQ(cpu->flags.rflags & 0x8D5u, kRetFlags & 0x8D5u)
            << engine << ": RET affects no flags";
        for (u8 r = 0; r < 8; ++r) {
            if (r == ESP) continue;
            EXPECT_EQ(cpu->gpr.get32(r), 0u) << engine << " GPR " << int(r);
        }
    }
}

std::vector<u8> ret_bytes(bool op16, std::optional<u16> imm) {
    std::vector<u8> code;
    if (op16) code.push_back(0x66);
    if (imm.has_value()) {
        code.push_back(0xC2);
        code.push_back(static_cast<u8>(*imm));
        code.push_back(static_cast<u8>(*imm >> 8));
    } else {
        code.push_back(0xC3);
    }
    return code;
}
}  // namespace

TEST_F(JitParityTest, RetImm16ReleasesTheFullUnsignedRange) {
    constexpr u32 kEsp = 0x10000;
    constexpr u32 kReturn = 0x0040'1234u;
    ASSERT_TRUE(m_memory->write_u32(kEsp, kReturn).is_ok());
    for (const u32 imm : {0u, 8u, 4087u, 4088u, 4091u, 4092u, 4095u, 4096u,
                          0x7FF8u, 0xFFFFu}) {
        SCOPED_TRACE(::testing::Message() << "ret 0x" << std::hex << imm);
        const RetRun run = run_ret32_both(
            *m_memory, ret_bytes(false, static_cast<u16>(imm)), kEsp);
        expect_ret32_completed(run, kEsp + 4u + imm, kReturn);
    }
}

TEST_F(JitParityTest, RetWithOperandSizePrefixPopsSixteenBitIp) {
    // 66H: a 2-byte pop, EIP zero-extended from it, ESP += 2 (+ imm16).
    constexpr u32 kEsp = 0x10000;
    ASSERT_TRUE(m_memory->write_u32(kEsp, 0xBEEF'4321u).is_ok());
    {
        SCOPED_TRACE("66 ret");
        const RetRun run =
            run_ret32_both(*m_memory, ret_bytes(true, std::nullopt), kEsp);
        expect_ret32_completed(run, kEsp + 2u, 0x4321u);
    }
    for (const u32 imm : {0u, 8u, 4093u, 4094u, 4095u, 4096u, 0xFFFFu}) {
        SCOPED_TRACE(::testing::Message() << "66 ret 0x" << std::hex << imm);
        const RetRun run = run_ret32_both(
            *m_memory, ret_bytes(true, static_cast<u16>(imm)), kEsp);
        expect_ret32_completed(run, kEsp + 2u + imm, 0x4321u);
    }
}

TEST_F(JitParityTest, RetImm16StackAdjustAtTheTopOfTheAddressSpace) {
    // StackAddressSize = 32: ESP := ESP + SRC, a 32-bit sum. First the
    // highest stack a large-address-aware process can own (user space ends at
    // 0xFFFEFFFF): the largest release lands exactly on 0xFFFFFFFF. Then the
    // top page, where the sum wraps modulo 2^32.
    ASSERT_TRUE(m_memory->map_region(0xFFFE'0000u, 0x10000,
                                     MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(m_memory->map_region(0xFFFF'F000u, 0x1000,
                                     MemoryProtection::ReadWrite).is_ok());
    const std::array<u8, 2> ip16{0x21, 0x43};
    {
        SCOPED_TRACE("ret 0xFFFF at ESP=0xFFFEFFFC");
        ASSERT_TRUE(m_memory->write_u32(0xFFFE'FFFCu, 0x0040'1234u).is_ok());
        const RetRun run = run_ret32_both(
            *m_memory, ret_bytes(false, u16{0xFFFF}), 0xFFFE'FFFCu);
        // 0xFFFEFFFC + 4 + 0xFFFF = 0xFFFFFFFF
        expect_ret32_completed(run, 0xFFFF'FFFFu, 0x0040'1234u);
    }
    {
        SCOPED_TRACE("66 ret 0xFFFF at ESP=0xFFFEFFFE");
        ASSERT_TRUE(m_memory->write_bytes(0xFFFE'FFFEu, ip16.data(),
                                          ip16.size()).is_ok());
        const RetRun run = run_ret32_both(
            *m_memory, ret_bytes(true, u16{0xFFFF}), 0xFFFE'FFFEu);
        // 0xFFFEFFFE + 2 + 0xFFFF = 0xFFFFFFFF
        expect_ret32_completed(run, 0xFFFF'FFFFu, 0x4321u);
    }
    {
        SCOPED_TRACE("ret 0x100 at ESP=0xFFFFFFF8");
        ASSERT_TRUE(m_memory->write_u32(0xFFFF'FFF8u, 0x0040'1234u).is_ok());
        const RetRun run = run_ret32_both(
            *m_memory, ret_bytes(false, u16{0x100}), 0xFFFF'FFF8u);
        // 0xFFFFFFF8 + 4 + 0x100 = 0x1'0000'00FC
        expect_ret32_completed(run, 0x0000'00FCu, 0x0040'1234u);
    }
    {
        SCOPED_TRACE("66 ret 0x10 at ESP=0xFFFFFFFE");
        ASSERT_TRUE(m_memory->write_bytes(0xFFFF'FFFEu, ip16.data(),
                                          ip16.size()).is_ok());
        const RetRun run = run_ret32_both(
            *m_memory, ret_bytes(true, u16{0x10}), 0xFFFF'FFFEu);
        // 0xFFFFFFFE + 2 + 0x10 = 0x1'0000'0010
        expect_ret32_completed(run, 0x0000'0010u, 0x4321u);
    }
}

TEST_F(JitParityTest, RetImm16PopFaultCommitsNothing) {
    // #PF on the pop: the instruction does not complete, so ESP and EIP keep
    // their values whatever the release count, and Windows reports
    // STATUS_ACCESS_VIOLATION at the RET.
    constexpr u32 kUnmappedStack = 0x5000'0000u;
    for (const bool op16 : {false, true}) {
        for (const u32 imm : {8u, 4088u, 0xFFFFu}) {
            SCOPED_TRACE(::testing::Message() << (op16 ? "66 " : "")
                         << "ret 0x" << std::hex << imm);
            const RetRun run = run_ret32_both(
                *m_memory, ret_bytes(op16, static_cast<u16>(imm)),
                kUnmappedStack);
            EXPECT_EQ(run.interp_result, ExecResult::Exception);
            // Both engines: STATUS_ACCESS_VIOLATION, a read (0) of the stack
            // slot -- ExceptionInformation[0] and [1].
            EXPECT_EQ(cpu_exception_to_status(run.interp_exception),
                      0xC0000005u);
            EXPECT_EQ(run.interp_exception_error, 0u);
            EXPECT_EQ(run.interp_exception_addr, u64{kUnmappedStack});
            EXPECT_TRUE(run.jit.pending_raise.active);
            EXPECT_EQ(run.jit.pending_raise.code, 0xC0000005u);
            ASSERT_GE(run.jit.pending_raise.num_args, 2u);
            EXPECT_EQ(run.jit.pending_raise.args[0], 0u);
            EXPECT_EQ(run.jit.pending_raise.args[1], u64{kUnmappedStack});
            for (const CpuState* cpu : {&run.interp, &run.jit}) {
                EXPECT_EQ(cpu->gpr.get32(ESP), kUnmappedStack);
                EXPECT_EQ(cpu->rip, 0x1000u);
            }
        }
    }
}

TEST_F(JitParityTest, RepPrefixedRetIsAPlainNearReturn) {
    // F3 on RET has no meaning (Intel SDM Vol. 2 REP: string instructions
    // only): F3 C3 and F3 C2 iw are plain near returns.
    constexpr u32 kEsp = 0x10000;
    constexpr u32 kReturn = 0x0040'1234u;
    ASSERT_TRUE(m_memory->write_u32(kEsp, kReturn).is_ok());
    {
        SCOPED_TRACE("rep ret");
        const RetRun run = run_ret32_both(*m_memory, {0xF3, 0xC3}, kEsp);
        expect_ret32_completed(run, kEsp + 4u, kReturn);
    }
    {
        SCOPED_TRACE("rep ret 0xFFC");
        const RetRun run =
            run_ret32_both(*m_memory, {0xF3, 0xC2, 0xFC, 0x0F}, kEsp);
        expect_ret32_completed(run, kEsp + 4u + 0xFFCu, kReturn);
    }
}

// ===========================================================================
// Indirect CALL through an absolute address — the IAT call form MSVC emits
// for every imported function (`call dword ptr [__imp__Fn]`, FF /2 mod=00
// rm=101, 6 bytes). The return address pushed must be instruction_start + 6;
// pushing +7 resumes one byte into the next instruction.
// ===========================================================================

TEST_F(JitParityTest, CallIndirectAbsoluteReturnAddressIsInstructionEnd) {
    // check_parity() is for straight-line code — a CALL ends the block, so
    // assert the emitted behaviour directly instead.
    u8 code[] = { 0xFF, 0x15, 0x00, 0x20, 0x00, 0x00 };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    ASSERT_TRUE(m_memory->write_u32(0x2000, 0x3000));

    BasicBlock block = build_block(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 1u);

    CpuState jcpu;
    init_cpu(jcpu);
    JitCompiler jit(*m_memory);
    CompiledBlock* cb = jit.compile(block);
    ASSERT_NE(cb, nullptr);
    (void)test::execute_like_dispatcher(jit, *cb, jcpu);

    const u32 esp = jcpu.gpr.get32(ESP);
    const u32 pushed = m_memory->read_u32(esp).value_or(0);
    EXPECT_EQ(pushed, 0x1006u) << "return address must be instruction_start + 6";
    EXPECT_EQ(jcpu.rip, 0x3000u) << "call target";
}
