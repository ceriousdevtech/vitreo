// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): stack operations and stack faults
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

// ============================================================================
// PUSH / POP parity (stack behavior)
// ============================================================================

TEST_F(JitParityTest, PushPop) {
    check_parity({0x50, 0x5B}, // PUSH EAX; POP EBX
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xDEADBEEF);
            cpu.gpr.set32(EBX, 0);
        },
        false);
}

TEST_F(JitParityTest, PushImm32) {
    // PUSH 0x12345678 — 68 78563412
    check_parity({0x68, 0x78, 0x56, 0x34, 0x12}, {}, false);
}

TEST_F(JitParityTest, PushPopMultiple) {
    // PUSH EAX; PUSH EBX; POP EBX; POP EAX
    check_parity({0x50, 0x53, 0x5B, 0x58},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x11111111);
            cpu.gpr.set32(EBX, 0x22222222);
        },
        false);
}

TEST_F(JitParityTest, PopRm32Reg) {
    // PUSH EBX; POP ECX via 8F /0 (8F C1)
    check_parity({0x53, 0x8F, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0xA1B2C3D4u);
            cpu.gpr.set32(ECX, 0);
        },
        false);
}

TEST_F(JitParityTest, PopRm32Memory) {
    // PUSH 0x11223344; POP DWORD PTR [EBX]
    // Destination at 0x2000 (inside mapped RWX region).
    check_parity({0x68, 0x44, 0x33, 0x22, 0x11, 0x8F, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x2000);
        },
        false);
}

TEST_F(JitParityTest, EnterLevel0AllocatesFrame) {
    // ENTER 0x0020, 0 — C8 20 00 00
    check_parity({0xC8, 0x20, 0x00, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBP, 0xDEADBEEFu);
        },
        false);
}

TEST_F(JitParityTest, EnterLevel1PushesFrameTemp) {
    // ENTER 0x0010, 1 — C8 10 00 01
    check_parity({0xC8, 0x10, 0x00, 0x01},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBP, 0xCAFEBABEu);
        },
        false);
}

TEST_F(JitParityTest, EnterLevel2CopiesDisplay) {
    // Parent frame: ENTER 0, 1 then child ENTER 0, 2
    // C8 00 00 01 ; C8 00 00 02
    check_parity({0xC8, 0x00, 0x00, 0x01, 0xC8, 0x00, 0x00, 0x02},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBP, 0x11112222u);
        },
        false);
}

TEST_F(JitParityTest, PushaPopaRoundTrip) {
    // PUSHAD; POPAD — 60 61. ESP slot is original ESP; POPAD skips restoring ESP.
    check_parity({0x60, 0x61},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x11111111u);
            cpu.gpr.set32(ECX, 0x22222222u);
            cpu.gpr.set32(EDX, 0x33333333u);
            cpu.gpr.set32(EBX, 0x44444444u);
            cpu.gpr.set32(EBP, 0x55555555u);
            cpu.gpr.set32(ESI, 0x66666666u);
            cpu.gpr.set32(EDI, 0x77777777u);
        },
        false);
}

TEST_F(JitParityTest, PushaStoresOriginalEsp) {
    // PUSHAD; POP EAX… then read the ESP slot (5th dword from top after push).
    // After PUSHAD, stack top is EDI; ESP slot is at [ESP+16].
    // MOV EAX,[ESP+16] — 8B 44 24 10
    check_parity({0x60, 0x8B, 0x44, 0x24, 0x10},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xA1A1A1A1u);
            cpu.gpr.set32(ECX, 0xB2B2B2B2u);
            cpu.gpr.set32(EDX, 0xC3C3C3C3u);
            cpu.gpr.set32(EBX, 0xD4D4D4D4u);
            cpu.gpr.set32(EBP, 0xE5E5E5E5u);
            cpu.gpr.set32(ESI, 0xF6F6F6F6u);
            cpu.gpr.set32(EDI, 0x17171717u);
        },
        false);
}

// ============================================================================
// Stack accesses that fault outside the stack guard are page faults
//
// tools/oracle_probe/stack_access_fault_probe.expected (Windows, WOW64): a
// PUSH-class write or POP-class read whose stack slot is inaccessible is
// STATUS_ACCESS_VIOLATION with ExceptionInformation[0] = 1 for the write /
// 0 for the read and [1] = the slot -- never STATUS_STACK_OVERFLOW, which is
// the stack guard's alone (stack_overflow_probe.expected). Reported at the
// instruction. The interpreter used to report every one of these as
// StackFault (STATUS_STACK_OVERFLOW).
// ============================================================================

namespace {

struct StackFaultRun {
    CpuState interp{};
    CpuState jit{};
    ExecResult interp_result{ExecResult::Continue};
    CpuException interp_exception{CpuException::None};
    u32 interp_exception_error{0};
    u64 interp_exception_addr{0};
    bool jit_compiled{false};
};

StackFaultRun run_stack_fault32_both(GuestMemory& mem, const std::vector<u8>& code,
                                     const std::function<void(CpuState&)>& setup) {
    StackFaultRun run;
    EXPECT_TRUE(mem.write_bytes(0x1000, code.data(), code.size()).is_ok());
    BasicBlock block;
    block.start_addr = 0x1000;
    MemoryByteReader reader(code.data(), code.size());
    DecodedInstruction inst;
    EXPECT_EQ(decode_instruction(reader, DecodeMode::Mode32, inst),
              DecodeError::Success);
    block.instructions.push_back(inst);
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = 1;
    const auto init = [&](CpuState& cpu) {
        cpu.init_protected_mode();
        cpu.gpr.set32(ESP, 0x10000);
        cpu.rip = 0x1000;
        setup(cpu);
    };
    init(run.interp);
    {
        Interpreter interp(run.interp, mem);
        run.interp_result = interp.step();
        run.interp_exception = interp.context().exception;
        run.interp_exception_error = interp.context().exception_error;
        run.interp_exception_addr = interp.context().exception_addr;
    }
    init(run.jit);
    JitCompiler jit(mem);
    CompiledBlock* cb = jit.compile(block);
    run.jit_compiled = cb != nullptr;
    if (cb != nullptr) (void)test::execute_like_dispatcher(jit, *cb, run.jit);
    return run;
}

}  // namespace

TEST_F(JitParityTest, StackAccessFaultsOutsideTheGuardAreAccessViolations) {
    constexpr u32 kUnmapped = 0x5000'0000u;
    constexpr u32 kData = 0x2000u;
    ASSERT_TRUE(m_memory->write_u32(kData, 0x1234u).is_ok());
    struct Form {
        const char* name;
        std::vector<u8> code;
        bool write;      // the faulting access is the push's write
        bool via_ebp;    // LEAVE: EBP names the unmapped slot
        bool esp_kept;   // the oracle's stack pointer (ctxsp=+0) is asserted
        bool native;     // Tier 0 lowers it
    };
    const Form forms[] = {
        {"push eax", {0x50}, true, false, true, true},
        {"push imm32", {0x68, 0x44, 0x33, 0x22, 0x11}, true, false, true, true},
        {"push [ebx]", {0xFF, 0x33}, true, false, true, true},
        {"pushfd", {0x9C}, true, false, true, true},
        {"call rel32", {0xE8, 0x00, 0x00, 0x00, 0x00}, true, false, true, true},
        {"call eax", {0xFF, 0xD0}, true, false, true, true},
        {"call [ebx]", {0xFF, 0x13}, true, false, true, true},
        {"enter 16,0", {0xC8, 0x10, 0x00, 0x00}, true, false, true, true},
        {"pushad", {0x60}, true, false, true, true},
        {"pop eax", {0x58}, false, false, true, true},
        {"pop [ebx]", {0x8F, 0x03}, false, false, true, true},
        {"popfd", {0x9D}, false, false, true, true},
        {"popad", {0x61}, false, false, true, true},
        // LEAVE is one instruction: Windows reports its starting ESP
        // (ctxsp=+0), not ESP := EBP.
        {"leave", {0xC9}, false, true, true, true},
    };
    for (const Form& f : forms) {
        SCOPED_TRACE(f.name);
        const u32 esp = f.via_ebp ? 0x8000u : kUnmapped;
        const StackFaultRun run = run_stack_fault32_both(
            *m_memory, f.code, [&](CpuState& cpu) {
                cpu.gpr.set32(ESP, esp);
                cpu.gpr.set32(EBP, f.via_ebp ? kUnmapped : 0x9000u);
                cpu.gpr.set32(EAX, 0x1000u);
                cpu.gpr.set32(EBX, kData);
            });
        const u64 slot = f.write ? u64{kUnmapped} - 4u : u64{kUnmapped};
        EXPECT_EQ(run.interp_result, ExecResult::Exception);
        EXPECT_EQ(cpu_exception_to_status(run.interp_exception), 0xC0000005u);
        EXPECT_EQ(run.interp_exception_error, f.write ? 1u : 0u);
        EXPECT_EQ(run.interp_exception_addr, slot);
        ASSERT_EQ(run.jit_compiled, f.native);
        if (run.jit_compiled) {
            EXPECT_TRUE(run.jit.pending_raise.active);
            EXPECT_EQ(run.jit.pending_raise.code, 0xC0000005u);
            ASSERT_GE(run.jit.pending_raise.num_args, 2u);
            EXPECT_EQ(run.jit.pending_raise.args[0], f.write ? 1u : 0u);
            EXPECT_EQ(run.jit.pending_raise.args[1], slot);
        }
        for (const CpuState* cpu : {&run.interp, &run.jit}) {
            if (cpu == &run.jit && !run.jit_compiled) continue;
            const char* engine = cpu == &run.jit ? "jit" : "interp";
            EXPECT_EQ(cpu->rip, 0x1000u) << engine << ": reported at the instruction";
            if (f.esp_kept) EXPECT_EQ(cpu->gpr.get32(ESP), esp) << engine;
        }
    }
}

TEST_F(JitParityTest, PopadFaultingOnItsFifthSlotKeepsEspButNotTheEarlierPops) {
    // stack_access_fault_probe.expected, `popad partial`: four slots read,
    // the fifth (EBX) faults. Windows on Intel reports the STARTING ESP, but
    // EDI, ESI and EBP hold the values their slots supplied; EBX..EAX keep
    // theirs. The record names the fifth slot. The mapped region ends at
    // 0x11000.
    constexpr u32 kEsp = 0x11000u - 16u;
    for (u32 i = 0; i < 4; ++i) {
        ASSERT_TRUE(m_memory->write_u32(kEsp + 4 * i, 0xAAAA0000u + i).is_ok());
    }
    const StackFaultRun run = run_stack_fault32_both(
        *m_memory, {0x61}, [&](CpuState& cpu) {
            cpu.gpr.set32(ESP, kEsp);
            for (u8 r = 0; r < 8; ++r) {
                if (r != ESP) cpu.gpr.set32(r, 0x1000u + r);
            }
        });
    EXPECT_EQ(run.interp_result, ExecResult::Exception);
    EXPECT_EQ(cpu_exception_to_status(run.interp_exception), 0xC0000005u);
    EXPECT_EQ(run.interp_exception_error, 0u);
    EXPECT_EQ(run.interp_exception_addr, 0x11000u);
    ASSERT_TRUE(run.jit_compiled) << "Tier 0 lowers POPA";
    EXPECT_TRUE(run.jit.pending_raise.active);
    EXPECT_EQ(run.jit.pending_raise.code, 0xC0000005u);
    ASSERT_GE(run.jit.pending_raise.num_args, 2u);
    EXPECT_EQ(run.jit.pending_raise.args[0], 0u);
    EXPECT_EQ(run.jit.pending_raise.args[1], 0x11000u);
    // Pop order EDI, ESI, EBP, (ESP slot), EBX, EDX, ECX, EAX.
    const u32 popped[8] = {
        0x1000u + EAX, 0x1000u + ECX, 0x1000u + EDX, 0x1000u + EBX,
        0,             0xAAAA0002u,   0xAAAA0001u,   0xAAAA0000u,
    };
    for (const CpuState* cpu : {&run.interp, &run.jit}) {
        const char* engine = cpu == &run.jit ? "jit" : "interp";
        EXPECT_EQ(cpu->gpr.get32(ESP), kEsp) << engine;
        for (u8 r = 0; r < 8; ++r) {
            if (r == ESP) continue;
            EXPECT_EQ(cpu->gpr.get32(r), popped[r])
                << engine << " register " << static_cast<int>(r);
        }
        EXPECT_EQ(cpu->rip, 0x1000u) << engine;
    }
}

// PUSH ESP (0x54) pushes the value ESP held *before* the decrement, so the
// pushed dword must equal the original ESP, not ESP-4. MSVC uses this to hand
// a callee the address of a just-reserved stack block ("sub esp,N; push esp;
// call f"); an off-by-4 there shifts every field the callee writes and only
// surfaces much later as a corrupted object. POP ESP is the mirror case: ESP
// must end up holding the popped value, not value+4.
TEST_F(JitParityTest, PushEspPushesPreDecrementValue) {
    check_parity({0x54}, {}, false); // PUSH ESP
}

TEST_F(JitParityTest, PushEspAfterStackReservationRoundTrips) {
    // sub esp,0x20 ; push esp ; pop eax  -> EAX must equal the reserved block
    // address (ESP after the sub), which is the MSVC sret idiom.
    check_parity({0x83, 0xEC, 0x20, 0x54, 0x58}, {}, false);
}

TEST_F(JitParityTest, PopEspTakesPoppedValue) {
    // push 0x2000 ; pop esp  -> ESP must be exactly 0x2000
    check_parity({0x68, 0x00, 0x20, 0x00, 0x00, 0x5C}, {}, false);
}

TEST_F(JitParityTest, XchgEaxEspThenStoreThroughNewEsp) {
    // Intel SDM Vol. 2 XCHG r32, r32: operands swap. MSVC __chkstk does
    // `xchg esp, eax` then `mov [esp], eax` to park the return address on
    // the newly reserved stack. A stale ESP-frame cache would write the
    // return slot at the OLD ESP and leave the new frame's cookie check
    // reading a smash (FAST_FAIL_STACK_COOKIE_CHECK_FAILURE).
    check_parity({
        0x94,             // xchg eax, esp
        0x8B, 0x00,       // mov eax, [eax]
        0x89, 0x04, 0x24, // mov [esp], eax
    }, [](CpuState& cpu) {
        cpu.gpr.set32(ESP, 0x9000);
        cpu.gpr.set32(EAX, 0x7000);
        cpu.gpr.set32(ECX, 0x12345678);
    }, false);
}

TEST_F(JitParityTest, MsvcChkStkShortPathMatchesInterpreter) {
    // Qt5Gui __chkstk short path (no page-probe loop): after `call`, EAX is
    // the reservation and the helper returns with ESP reduced by that amount
    // and the original return address sitting at the new top.
    check_parity({
        0x51,                         // push ecx
        0x8D, 0x4C, 0x24, 0x04,       // lea ecx, [esp+4]
        0x2B, 0xC8,                   // sub ecx, eax
        0x1B, 0xC0,                   // sbb eax, eax
        0xF7, 0xD0,                   // not eax
        0x23, 0xC8,                   // and ecx, eax
        0x8B, 0xC1,                   // mov eax, ecx
        0x59,                         // pop ecx
        0x94,                         // xchg eax, esp
        0x8B, 0x00,                   // mov eax, [eax]
        0x89, 0x04, 0x24,             // mov [esp], eax
    }, [](CpuState& cpu) {
        cpu.gpr.set32(ESP, 0x9000);
        cpu.gpr.set32(EAX, 0x2034);
        cpu.gpr.set32(ECX, 0xA5A5A5A5);
    }, false);
}

TEST_F(JitParityTest, CmpMemThenCmovlSelectsSignedMin) {
    // Intel SDM Vol. 2 CMOVL: move iff SF != OF. MSVC qMin is
    // `cmp ecx, [eax]; cmovl eax, edx` then `[eax]` — clamp to 0x800.
    // A always-taken CMOVL returns the unclamped width and overflows the
    // 2048-pixel stack buffer in Qt's convert_generic.
    ASSERT_TRUE(m_memory->write_u32(0x3000, 0x1000u).is_ok());
    ASSERT_TRUE(m_memory->write_u32(0x3004, 0x800u).is_ok());
    check_parity({
        0xBA, 0x00, 0x30, 0x00, 0x00, // mov edx, 0x3000
        0xB8, 0x04, 0x30, 0x00, 0x00, // mov eax, 0x3004
        0x8B, 0x0A,                   // mov ecx, [edx]
        0x3B, 0x08,                   // cmp ecx, [eax]
        0x0F, 0x4C, 0xC2,             // cmovl eax, edx
        0x8B, 0x00,                   // mov eax, [eax]
    }, {}, false);

    ASSERT_TRUE(m_memory->write_u32(0x3000, 0x100u).is_ok());
    ASSERT_TRUE(m_memory->write_u32(0x3004, 0x800u).is_ok());
    check_parity({
        0xBA, 0x00, 0x30, 0x00, 0x00,
        0xB8, 0x04, 0x30, 0x00, 0x00,
        0x8B, 0x0A,
        0x3B, 0x08,
        0x0F, 0x4C, 0xC2,
        0x8B, 0x00,
    }, {}, false);
}

TEST_F(JitParityTest, MsvcChkStkProbeTouchesMatchInterpreter) {
    // Probe body only: `sub eax, 0x1000; test dword [eax], eax`.
    // Intel SDM Vol. 2 TEST r/m32, r32: AND, set SF/ZF/PF, clear OF/CF,
    // destination is not written. Two touches cover a 0x2034 reservation
    // from a page-aligned ESP.
    check_parity({
        0x2D, 0x00, 0x10, 0x00, 0x00, // sub eax, 0x1000
        0x85, 0x00,                   // test dword [eax], eax
        0x2D, 0x00, 0x10, 0x00, 0x00, // sub eax, 0x1000
        0x85, 0x00,                   // test dword [eax], eax
    }, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0x9000);
        cpu.gpr.set32(ECX, 0x6FCC);
        // Distinct from EAX so a TEST store-through-reg would show up.
        cpu.gpr.set32(EDX, 0x11111111);
    }, true);
}

TEST_F(JitParityTest, EspEbpDispLoadsMatchInterpreter) {
    // push imm; mov eax,[esp]; mov ecx,[esp+4]; mov ebp,esp; mov edx,[ebp];
    // mov ebx,[ebp+4]; mov esi,[ebp-4] after a second push.
    check_parity({
        0x68, 0x11, 0x11, 0x11, 0x11, // push 0x11111111
        0x68, 0x22, 0x22, 0x22, 0x22, // push 0x22222222
        0x8B, 0x04, 0x24,             // mov eax, [esp]
        0x8B, 0x4C, 0x24, 0x04,       // mov ecx, [esp+4]
        0x89, 0xE5,                   // mov ebp, esp
        0x8B, 0x55, 0x00,             // mov edx, [ebp]
        0x8B, 0x5D, 0x04,             // mov ebx, [ebp+4]
        0x68, 0xDD, 0xCC, 0xBB, 0xAA, // push 0xAABBCCDD
        0x8B, 0x75, 0xFC,             // mov esi, [ebp-4]
    }, [](CpuState& cpu) {
        cpu.gpr.set32(ESP, 0x8000);
        cpu.gpr.set32(EBP, 0x12345678);
    }, false);
}


TEST_F(JitParityTest, MsvcFramePrologueAndLeaveRestoreCallerEbp) {
    // push ebp; mov ebp,esp; sub esp,0x14;
    // push ebx; push esi; push edi; pop edi; pop esi; pop ebx; leave
    //
    // This is the frame shape emitted by MSVC constructors. LEAVE must load
    // the caller EBP saved by PUSH EBP, even when the frame used local storage
    // and preserved the other nonvolatile registers.
    check_parity({
        0x55,
        0x8B, 0xEC,
        0x83, 0xEC, 0x14,
        0x53, 0x56, 0x57,
        0x5F, 0x5E, 0x5B,
        0xC9,
    }, [](CpuState& cpu) {
        cpu.gpr.set32(EBP, 0x2468ACE0);
        cpu.gpr.set32(EBX, 0x11111111);
        cpu.gpr.set32(ESI, 0x22222222);
        cpu.gpr.set32(EDI, 0x33333333);
    }, false);
}
