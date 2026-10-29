// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): LEA, RET imm16,
// stack faults and non-canonical branches.

#include "jit_parity_64_fixture.hpp"

// ============================================================================
// 64-bit LEA
// ============================================================================

TEST_F(JitParity64Test, LeaR64BaseDisp) {
    // LEA RAX, [RBX+0x10] — 48 8D 43 10
    check_parity({0x48, 0x8D, 0x43, 0x10},
        [](CpuState& cpu) { cpu.gpr.set64(RBX, 0x100000000ull); },
        false);
}

TEST_F(JitParity64Test, LeaR64ExtendedBaseScaledIndex) {
    // LEA RCX, [R9+RAX*4] — 49 8D 0C 81
    check_parity({0x49, 0x8D, 0x0C, 0x81},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x35);
            cpu.gpr.set64(static_cast<u8>(RegIndex::R9), 0x121400298000ull);
        },
        false);
}

TEST_F(JitParity64Test, HighRipRelativeLeaThenRet) {
    // Exact instruction shape used by SteamUI's startup helper:
    //   7ffa015d5df0: lea rax, [rip+0x00a7f909]
    //   7ffa015d5df7: ret
    //
    // Keep the real high guest VA in this regression. Low-address LEA tests
    // cannot catch truncation in either next-RIP calculation or RET targets.
    constexpr GuestAddr code_addr = 0x7ffa015d5df0ull;
    constexpr GuestAddr code_page = code_addr & ~GuestAddr{0xfff};
    constexpr GuestAddr stack_addr = 0x10000;
    constexpr u64 return_addr = 0x7ffa01569ed9ull;
    constexpr u64 lea_result = 0x7ffa02055700ull;
    const u8 code[] = {
        0x48, 0x8D, 0x05, 0x09, 0xF9, 0xA7, 0x00, // LEA RAX,[RIP+disp32]
        0xC3                                            // RET
    };

    ASSERT_TRUE(m_memory->map_region(
        code_page, 0x1000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_bytes(code_addr, code, sizeof(code)));
    ASSERT_TRUE(m_memory->write_u64(stack_addr, return_addr));

    BasicBlock block = build_block(code, sizeof(code));
    block.start_addr = code_addr;

    CpuState cpu;
    init_cpu(cpu);
    cpu.rip = code_addr;
    cpu.gpr.set64(RSP, stack_addr);

    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    EXPECT_EQ(cpu.gpr.rax, lea_result);
    EXPECT_EQ(cpu.rip, return_addr);
    EXPECT_EQ(cpu.gpr.rsp, stack_addr + sizeof(u64));
}

// Intel SDM Vol. 2 RET (near, C2 iw): RIP := Pop() (8 bytes: "the operand
// size for near call (and all near branches) is forced to 64-bits"), then
// with StackAddressSize = 64 "RSP := RSP + SRC", SRC being the UNSIGNED imm16.
// Flags Affected: None. The adjustment is 8 + imm16, up to 65543 -- wider
// than the 12-bit field of ARM64 ADD (immediate), which the lowering once
// encoded it in, so every imm16 >= 4088 popped (8 + imm16) mod 4096 bytes.
// The expected values below come from that pseudocode, not from either engine.
TEST_F(JitParity64Test, RetImm16ReleasesTheFullUnsignedRange) {
    constexpr u64 kRsp = 0x10000;
    constexpr u64 kReturn = 0x00007FF6'1234'5678ull;
    constexpr u64 kFlags = 0x202u | 0x8D5u;   // CF PF AF ZF SF OF set
    for (const u32 imm : {0u, 8u, 4087u, 4088u, 4095u, 4096u, 0x7FF8u,
                          0xFFFFu}) {
        SCOPED_TRACE(::testing::Message() << "ret 0x" << std::hex << imm);
        ASSERT_TRUE(m_memory->write_u64(kRsp, kReturn));
        const u8 rex_w = 0x48;   // REX.W changes nothing: still a 64-bit pop
        for (const bool with_rex : {false, true}) {
            m_jit = std::make_unique<JitCompiler>(*m_memory);
            std::vector<u8> code;
            if (with_rex) code.push_back(rex_w);
            code.push_back(0xC2);
            code.push_back(static_cast<u8>(imm));
            code.push_back(static_cast<u8>(imm >> 8));
            ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
            check_parity(code.data(), code.size(),
                [&](CpuState& cpu) {
                    cpu.gpr.set64(RSP, kRsp);
                    cpu.flags.rflags = kFlags;
                },
                /*check_flags=*/true, /*check_xmm=*/false,
                /*require_jit=*/true, /*check_af=*/true);
            ASSERT_TRUE(m_last_parity_ok);
            for (const CpuState* cpu : {&m_last_interp_cpu, &m_last_jit_cpu}) {
                const char* engine = cpu == &m_last_jit_cpu ? "jit" : "interp";
                EXPECT_EQ(cpu->gpr.rsp, kRsp + 8u + imm) << engine;
                EXPECT_EQ(cpu->rip, kReturn) << engine;
                EXPECT_EQ(cpu->flags.rflags & 0x8D5u, kFlags & 0x8D5u)
                    << engine << ": RET affects no flags";
            }
        }
    }
}

// 66H on a near RET in 64-bit mode: the operand size is forced to 64 bits for
// every near branch (Intel SDM Vol. 2 CALL, which Vitreo's GenuineIntel CPUID
// promises), so the prefix is ignored -- an 8-byte pop and RSP += 8 + imm16.
TEST_F(JitParity64Test, RetImm16With66PrefixStillPopsEightBytes) {
    constexpr u64 kRsp = 0x10000;
    constexpr u64 kReturn = 0x00007FF6'1234'5678ull;
    // -1 is the C3 form (66 C3); the others are 66 C2 iw.
    for (const i32 imm : {-1, 0, 4088, 0xFFFF}) {
        SCOPED_TRACE(::testing::Message() << "66 ret " << imm);
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        ASSERT_TRUE(m_memory->write_u64(kRsp, kReturn));
        std::vector<u8> code{0x66};
        if (imm < 0) {
            code.push_back(0xC3);
        } else {
            code.insert(code.end(), {0xC2, static_cast<u8>(imm),
                                     static_cast<u8>(imm >> 8)});
        }
        check_parity(code.data(), code.size(),
            [&](CpuState& cpu) { cpu.gpr.set64(RSP, kRsp); },
            /*check_flags=*/true, /*check_xmm=*/false, /*require_jit=*/true);
        ASSERT_TRUE(m_last_parity_ok);
        const u64 release = 8u + static_cast<u64>(imm < 0 ? 0 : imm);
        for (const CpuState* cpu : {&m_last_interp_cpu, &m_last_jit_cpu}) {
            EXPECT_EQ(cpu->gpr.rsp, kRsp + release);
            EXPECT_EQ(cpu->rip, kReturn);
        }
    }
}

// F3 C3 ("rep ret") and F3 C2 iw: REP has no meaning on RET (Intel SDM Vol. 2
// REP: it applies to string instructions only), so these are plain near
// returns. Tier 1 declines the prefix; Tier 0 and the interpreter ignore it.
TEST_F(JitParity64Test, RepPrefixedRetIsAPlainNearReturn) {
    constexpr u64 kRsp = 0x10000;
    constexpr u64 kReturn = 0x00007FF6'1234'5678ull;
    const std::vector<std::vector<u8>> forms = {
        {0xF3, 0xC3}, {0xF3, 0xC2, 0x10, 0x00}, {0xF3, 0xC2, 0xF8, 0x0F}};
    const u64 releases[] = {8u, 8u + 0x10u, 8u + 0xFF8u};
    for (usize i = 0; i < forms.size(); ++i) {
        SCOPED_TRACE(::testing::Message() << "form " << i);
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        ASSERT_TRUE(m_memory->write_u64(kRsp, kReturn));
        check_parity(forms[i].data(), forms[i].size(),
            [&](CpuState& cpu) { cpu.gpr.set64(RSP, kRsp); },
            /*check_flags=*/true, /*check_xmm=*/false, /*require_jit=*/true);
        ASSERT_TRUE(m_last_parity_ok);
        for (const CpuState* cpu : {&m_last_interp_cpu, &m_last_jit_cpu}) {
            EXPECT_EQ(cpu->gpr.rsp, kRsp + releases[i]);
            EXPECT_EQ(cpu->rip, kReturn);
        }
    }
}

// A RET whose pop faults (#PF on an unmapped stack) completes nothing: RSP and
// RIP keep their values, whatever the release count would have been.
TEST_F(JitParity64Test, RetImm16PopFaultCommitsNothing) {
    constexpr u64 kUnmappedStack = 0x0000'4000'0000'0000ull;
    for (const u32 imm : {8u, 4088u, 0xFFFFu}) {
        SCOPED_TRACE(::testing::Message() << "ret 0x" << std::hex << imm);
        JitCompiler jit(*m_memory);
        const u8 code[] = {0xC2, static_cast<u8>(imm), static_cast<u8>(imm >> 8)};
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
        BasicBlock block = build_block(code, sizeof(code));
        CpuState icpu;
        init_cpu(icpu);
        icpu.gpr.set64(RSP, kUnmappedStack);
        {
            Interpreter interp(icpu, *m_memory);
            EXPECT_EQ(interp.step(), ExecResult::Exception);
            // The reference reports what Windows does for a #PF read:
            // STATUS_ACCESS_VIOLATION, ExceptionInformation[0] = 0 (read),
            // [1] = the faulting stack slot.
            const ExecContext& ictx = interp.context();
            EXPECT_EQ(cpu_exception_to_status(ictx.exception), 0xC0000005u);
            EXPECT_EQ(ictx.exception_error, 0u);
            EXPECT_EQ(ictx.exception_addr, kUnmappedStack);
        }
        CpuState jcpu;
        init_cpu(jcpu);
        jcpu.gpr.set64(RSP, kUnmappedStack);
        CompiledBlock* cb = jit.compile(block);
        ASSERT_NE(cb, nullptr);
        (void)test::execute_like_dispatcher(jit, *cb, jcpu);
        EXPECT_TRUE(jcpu.pending_raise.active);
        EXPECT_EQ(jcpu.pending_raise.code, 0xC0000005u)
            << "a #PF on the pop is STATUS_ACCESS_VIOLATION";
        ASSERT_GE(jcpu.pending_raise.num_args, 2u);
        EXPECT_EQ(jcpu.pending_raise.args[0], 0u);
        EXPECT_EQ(jcpu.pending_raise.args[1], kUnmappedStack);
        for (const CpuState* cpu : {&icpu, &jcpu}) {
            EXPECT_EQ(cpu->gpr.rsp, kUnmappedStack);
            EXPECT_EQ(cpu->rip, 0x1000u) << "the fault is reported at the RET";
        }
    }
}

// ============================================================================
// Stack accesses that fault outside the stack guard are page faults
//
// tools/oracle_probe/stack_access_fault_probe.expected (Windows, x64): a
// PUSH-class write or POP-class read whose stack slot is inaccessible is
// STATUS_ACCESS_VIOLATION with ExceptionInformation[0] = 1 for the write /
// 0 for the read and [1] = the slot -- never STATUS_STACK_OVERFLOW, which is
// the stack guard's alone (stack_overflow_probe.expected; the dispatcher maps
// an AV inside [DeallocationStack, StackLimit) to it). Reported at the
// instruction. The interpreter used to report every one of these as
// StackFault (STATUS_STACK_OVERFLOW).
// ============================================================================

TEST_F(JitParity64Test, StackAccessFaultsOutsideTheGuardAreAccessViolations) {
    // Two unmapped stacks: one far above any reservation (the JIT's bounds
    // check raises it in software), one inside the low 4 GiB, where a linear64
    // block's access reaches the host view and faults in hardware -- the path
    // that adds a pinned PUSH's decrement back (JitFlagRecovery::rsp_undo).
    for (const u64 kUnmapped : {u64{0x0000'4000'0000'0000ull}, u64{0x5000'0000u}}) {
    SCOPED_TRACE(::testing::Message() << "stack 0x" << std::hex << kUnmapped);
    constexpr u64 kData = 0x2000u;
    ASSERT_TRUE(m_memory->write_u64(kData, 0x1234u).is_ok());
    struct Form {
        const char* name;
        std::vector<u8> code;
        bool write;
        bool via_rbp;
        bool rsp_kept;
    };
    const Form forms[] = {
        {"push rax", {0x50}, true, false, true},
        {"push imm32", {0x68, 0x44, 0x33, 0x22, 0x11}, true, false, true},
        {"push [rbx]", {0xFF, 0x33}, true, false, true},
        {"pushfq", {0x9C}, true, false, true},
        {"call rel32", {0xE8, 0x00, 0x00, 0x00, 0x00}, true, false, true},
        {"call rax", {0xFF, 0xD0}, true, false, true},
        {"call [rbx]", {0xFF, 0x13}, true, false, true},
        {"enter 16,0", {0xC8, 0x10, 0x00, 0x00}, true, false, true},
        {"pop rax", {0x58}, false, false, true},
        {"pop [rbx]", {0x8F, 0x03}, false, false, true},
        {"popfq", {0x9D}, false, false, true},
        // LEAVE is one instruction: Windows reports its starting RSP
        // (ctxsp=+0), not RSP := RBP.
        {"leave", {0xC9}, false, true, true},
    };
    for (const Form& f : forms) {
        SCOPED_TRACE(f.name);
        const u64 rsp = f.via_rbp ? 0x8000u : kUnmapped;
        ASSERT_TRUE(m_memory->write_bytes(0x1000, f.code.data(), f.code.size()));
        BasicBlock block = build_block(f.code.data(), f.code.size());
        ASSERT_EQ(block.instruction_count, 1u);
        const auto setup = [&](CpuState& cpu) {
            init_cpu(cpu);
            cpu.gpr.set64(RSP, rsp);
            cpu.gpr.set64(RBP, f.via_rbp ? kUnmapped : 0x9000u);
            cpu.gpr.set64(RAX, 0x1000u);
            cpu.gpr.set64(RBX, kData);
        };
        const u64 slot = f.write ? kUnmapped - 8u : kUnmapped;
        CpuState icpu;
        setup(icpu);
        {
            Interpreter interp(icpu, *m_memory);
            EXPECT_EQ(interp.step(), ExecResult::Exception);
            const ExecContext& ictx = interp.context();
            EXPECT_EQ(cpu_exception_to_status(ictx.exception), 0xC0000005u);
            EXPECT_EQ(ictx.exception_error, f.write ? 1u : 0u);
            EXPECT_EQ(ictx.exception_addr, slot);
        }
        CpuState jcpu;
        setup(jcpu);
        JitCompiler jit(*m_memory);
        CompiledBlock* cb = jit.compile(block);
        ASSERT_NE(cb, nullptr) << "every form here has a native lowering";
        (void)test::execute_like_dispatcher(jit, *cb, jcpu);
        EXPECT_TRUE(jcpu.pending_raise.active);
        EXPECT_EQ(jcpu.pending_raise.code, 0xC0000005u);
        ASSERT_GE(jcpu.pending_raise.num_args, 2u);
        EXPECT_EQ(jcpu.pending_raise.args[0], f.write ? 1u : 0u);
        EXPECT_EQ(jcpu.pending_raise.args[1], slot);
        for (const CpuState* cpu : {&icpu, &jcpu}) {
            const char* engine = cpu == &jcpu ? "jit" : "interp";
            EXPECT_EQ(cpu->rip, 0x1000u) << engine << ": reported at the instruction";
            if (f.rsp_kept) EXPECT_EQ(cpu->gpr.rsp, rsp) << engine;
        }
    }
    }
}

// The LOOP family's target is a translation-time constant too: Tier 0 and the
// IR decline a non-canonical one, so the instruction runs in the interpreter
// (the block's delegate), which raises #GP(0) AT it with RCX untouched
// (InterpreterTest.LoopToNonCanonicalTargetIsGpAndKeepsRcx).
TEST_F(JitParity64Test, LoopToNonCanonicalTargetIsLeftToTheInterpreter) {
    constexpr u64 kPage = 0x0000'7FFF'FFFF'F000ull;
    if (!m_memory->map_region(kPage, 0x1000, MemoryProtection::ReadWriteExecute)) {
        GTEST_SKIP() << "the top of the user range is outside this memory model";
    }
    constexpr u64 kSite = kPage + 0xFF0;  // next = kSite + 2, +0x7F crosses 2^47
    struct Form {
        u8 op;
        u64 rcx;
        bool zf;  // every form below is TAKEN
    };
    const Form forms[] = {
        {0xE2, 5, false},  // loop
        {0xE1, 5, true},   // loope
        {0xE0, 5, false},  // loopne
        {0xE3, 0, false},  // jrcxz
    };
    for (const Form& f : forms) {
        for (const bool lead_nop : {false, true}) {
            SCOPED_TRACE(::testing::Message()
                         << "op 0x" << std::hex << static_cast<int>(f.op)
                         << (lead_nop ? " after a NOP" : " alone"));
            std::vector<u8> bytes;
            if (lead_nop) bytes.push_back(0x90);
            bytes.push_back(f.op);
            bytes.push_back(0x7F);
            const u64 start = lead_nop ? kSite - 1 : kSite;
            ASSERT_TRUE(m_memory->write_bytes(start, bytes.data(), bytes.size()));
            BasicBlock block;
            block.start_addr = start;
            block.is_64bit = true;
            MemoryByteReader reader(bytes.data(), bytes.size());
            while (reader.position() < bytes.size()) {
                DecodedInstruction inst;
                ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst),
                          DecodeError::Success);
                block.instructions.push_back(inst);
            }
            block.byte_length = static_cast<u32>(reader.position());
            block.instruction_count =
                static_cast<u32>(block.instructions.size());
            CpuState jcpu;
            init_cpu(jcpu);
            jcpu.rip = start;
            jcpu.gpr.set64(RCX, f.rcx);
            jcpu.flags.rflags = f.zf ? 0x246u : 0x202u;
            JitCompiler jit(*m_memory);
            CompiledBlock* cb = jit.compile(block);
            ASSERT_NE(cb, nullptr);
            (void)test::execute_like_dispatcher(jit, *cb, jcpu);
            EXPECT_TRUE(jcpu.pending_raise.active);
            EXPECT_EQ(jcpu.pending_raise.code, 0xC0000005u);
            ASSERT_GE(jcpu.pending_raise.num_args, 2u);
            EXPECT_EQ(jcpu.pending_raise.args[0], 0u);
            EXPECT_EQ(jcpu.pending_raise.args[1], ~u64{0});
            EXPECT_EQ(jcpu.rip, kSite) << "reported at the LOOP";
            EXPECT_EQ(jcpu.gpr.get64(RCX), f.rcx) << "RCX not decremented";
        }
    }
}

// POP r/m64 whose DESTINATION write faults: the slot read succeeds, the write
// does not, and nothing commits -- RSP included (Intel SDM Vol. 2 POP: the
// fault is precise). The EA uses the incremented RSP, so `pop [rsp+disp]`
// with disp = top - (RSP + 8) writes exactly `top`, the first unmapped byte
// (formed from the old RSP it would name a mapped byte and not fault).
TEST_F(JitParity64Test, PopRm64DestinationFaultCommitsNothing) {
    constexpr u64 kUnmapped = 0x5000'0000u;
    constexpr u64 kRsp = 0x10000u;
    ASSERT_TRUE(m_memory->write_u64(kRsp, 0x1122'3344'5566'7788ull).is_ok());
    u64 top = 0;
    for (u64 a = kRsp; a < 0x100000u; a += 0x1000u) {
        u8 b = 0;
        if (!m_memory->read_bytes(a, &b, 1)) {
            top = a;
            break;
        }
    }
    ASSERT_NE(top, 0u) << "no unmapped page above the stack";
    ASSERT_GT(top, kRsp + 8u);
    const u32 disp = static_cast<u32>(top - (kRsp + 8u));
    struct Form {
        const char* name;
        std::vector<u8> code;
        u64 addr;  // the faulting write
    };
    const Form forms[] = {
        {"pop [rbx]", {0x8F, 0x03}, kUnmapped},
        {"pop [rsp+disp32]",
         {0x8F, 0x84, 0x24, static_cast<u8>(disp), static_cast<u8>(disp >> 8),
          static_cast<u8>(disp >> 16), static_cast<u8>(disp >> 24)},
         top},
    };
    for (const Form& f : forms) {
        SCOPED_TRACE(f.name);
        ASSERT_TRUE(m_memory->write_bytes(0x1000, f.code.data(), f.code.size()));
        BasicBlock block = build_block(f.code.data(), f.code.size());
        ASSERT_EQ(block.instruction_count, 1u);
        const auto setup = [&](CpuState& cpu) {
            init_cpu(cpu);
            cpu.gpr.set64(RSP, kRsp);
            cpu.gpr.set64(RBX, kUnmapped);
        };
        CpuState icpu;
        setup(icpu);
        {
            Interpreter interp(icpu, *m_memory);
            EXPECT_EQ(interp.step(), ExecResult::Exception);
            const ExecContext& ictx = interp.context();
            EXPECT_EQ(cpu_exception_to_status(ictx.exception), 0xC0000005u);
            EXPECT_EQ(ictx.exception_error, 1u);
            EXPECT_EQ(ictx.exception_addr, f.addr);
        }
        CpuState jcpu;
        setup(jcpu);
        JitCompiler jit(*m_memory);
        CompiledBlock* cb = jit.compile(block);
        ASSERT_NE(cb, nullptr);
        (void)test::execute_like_dispatcher(jit, *cb, jcpu);
        EXPECT_TRUE(jcpu.pending_raise.active);
        EXPECT_EQ(jcpu.pending_raise.code, 0xC0000005u);
        ASSERT_GE(jcpu.pending_raise.num_args, 2u);
        EXPECT_EQ(jcpu.pending_raise.args[0], 1u);
        EXPECT_EQ(jcpu.pending_raise.args[1], f.addr);
        for (const CpuState* cpu : {&icpu, &jcpu}) {
            const char* engine = cpu == &jcpu ? "jit" : "interp";
            EXPECT_EQ(cpu->rip, 0x1000u) << engine;
            EXPECT_EQ(cpu->gpr.rsp, kRsp) << engine << ": RSP not committed";
        }
    }
}

// ============================================================================
// rel32 branches to a NON-CANONICAL target: #GP(0) at the branch in both
// engines
//
// tools/oracle_probe/noncanonical_branch_probe.expected: from the top of the
// user range a JMP/CALL rel32 or a taken Jcc rel32 can name 0x800000000000;
// Intel raises #GP(0) at the branch with nothing committed, reported as
// STATUS_ACCESS_VIOLATION, [0] = 0, [1] = 0xFFFFFFFFFFFFFFFF. The target is a
// translation-time constant, so the JIT leaves such a branch to the
// interpreter (the block's delegate), which raises it exactly
// (InterpreterTest.Rel32BranchToNonCanonicalTargetIsGpAtTheBranch). (The JIT's
// register/memory forms and RET are a recorded gap: .docs/cpu-subsystem.md,
// Near RET.)
// ============================================================================

TEST_F(JitParity64Test, Rel32BranchToNonCanonicalTargetIsGpAtTheBranch) {
    constexpr u64 kTop = 0x0000'7FFF'FFFE'0000ull;
    if (!m_memory->map_region(kTop, 0x10000, MemoryProtection::ReadWriteExecute)) {
        GTEST_SKIP() << "the top of the user range is outside this memory model";
    }
    constexpr u64 kSite = kTop + 0x10000 - 0x100;
    constexpr u64 kTarget = 0x0000'8000'0000'0000ull;
    struct Form {
        const char* name;
        std::vector<u8> op;
        bool zf;
        bool faults;
    };
    const Form forms[] = {
        {"jmp rel32", {0xE9}, false, true},
        {"call rel32", {0xE8}, false, true},
        {"jz rel32 taken", {0x0F, 0x84}, true, true},
        {"jnz rel32 not taken", {0x0F, 0x85}, true, false},
    };
    // A block of the branch alone (lead_nop false), or NOP + branch.
    const auto build = [&](const std::vector<u8>& code, bool lead_nop) {
        std::vector<u8> bytes;
        if (lead_nop) bytes.push_back(0x90);
        bytes.insert(bytes.end(), code.begin(), code.end());
        const u64 start = lead_nop ? kSite - 1 : kSite;
        EXPECT_TRUE(m_memory->write_bytes(start, bytes.data(), bytes.size()));
        BasicBlock block;
        block.start_addr = start;
        block.is_64bit = true;
        MemoryByteReader reader(bytes.data(), bytes.size());
        while (reader.position() < bytes.size()) {
            DecodedInstruction inst;
            EXPECT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst),
                      DecodeError::Success);
            block.instructions.push_back(inst);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count =
            static_cast<u32>(block.instructions.size());
        return block;
    };
    for (const Form& f : forms) {
        std::vector<u8> code = f.op;
        const u64 next = kSite + code.size() + 4;
        const u32 rel = static_cast<u32>(kTarget - next);
        for (int i = 0; i < 4; ++i) code.push_back(static_cast<u8>(rel >> (8 * i)));
        for (const bool lead_nop : {false, true}) {
            SCOPED_TRACE(::testing::Message()
                         << f.name << (lead_nop ? " after a NOP" : " alone"));
            const BasicBlock block = build(code, lead_nop);
            CpuState jcpu;
            init_cpu(jcpu);
            jcpu.rip = block.start_addr;
            jcpu.flags.rflags = f.zf ? 0x246u : 0x202u;
            const u64 rsp = jcpu.gpr.rsp;
            JitCompiler jit(*m_memory);
            CompiledBlock* cb = jit.compile(block);
            ASSERT_NE(cb, nullptr);
            (void)test::execute_like_dispatcher(jit, *cb, jcpu);
            if (f.faults) {
                EXPECT_TRUE(jcpu.pending_raise.active);
                EXPECT_EQ(jcpu.pending_raise.code, 0xC0000005u);
                ASSERT_GE(jcpu.pending_raise.num_args, 2u);
                EXPECT_EQ(jcpu.pending_raise.args[0], 0u);
                EXPECT_EQ(jcpu.pending_raise.args[1], ~u64{0});
                EXPECT_EQ(jcpu.rip, kSite) << "reported at the branch";
                EXPECT_EQ(jcpu.gpr.rsp, rsp) << "a #GP CALL pushes nothing";
            } else {
                EXPECT_FALSE(jcpu.pending_raise.active);
                EXPECT_EQ(jcpu.rip, next);
            }
        }
    }
}
