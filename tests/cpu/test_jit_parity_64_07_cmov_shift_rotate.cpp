// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): BSWAP, CMOVcc,
// PUSH imm, shifts, SETcc and ROL/ROR.

#include "jit_parity_64_fixture.hpp"

// ============================================================================
// 64-bit BSWAP / CMOVcc / PUSH/POP r/m64
// ============================================================================

TEST_F(JitParity64Test, NativePushRspStoresPredecrementValue) {
    // Intel SDM Vol. 2 PUSH: PUSH RSP stores the value of RSP before the
    // stack-pointer decrement.  This is the alias boundary for the pinned-RSP
    // lowering.
    check_native_parity({0x54, 0x58},  // push rsp; pop rax
        [](CpuState& cpu) {
            cpu.gpr.set64(RSP, 0x9000);
            cpu.gpr.set64(RAX, 0xAAAAAAAAAAAAAAAAull);
        },
        false);
}

TEST_F(JitParity64Test, NativePushUsesPinnedAndExtendedRegisterValues) {
    check_native_parity({
            0x53,              // push rbx (pinned source)
            0x59,              // pop rcx
            0x41, 0x57,        // push r15 (CpuState source)
            0x58,              // pop rax
            0x53,              // push rbx
            0x41, 0x5F,        // pop r15 (CpuState destination)
        },
        [](CpuState& cpu) {
            cpu.gpr.set64(RBX, 0x0123456789ABCDEFull);
            cpu.gpr.set64(R15, 0xFEDCBA9876543210ull);
        },
        false);
}

TEST_F(JitParity64Test, Mov32ZeroExtendsBeforeShiftAndTableAdd) {
    // D3DCompiler_42 token-table address: r15 = zext(r9d) << 4 + [r8+0x10].
    // A stale upper half in R15 must not survive the 32-bit MOV.
    const u64 table = 0x4000;
    ASSERT_TRUE(m_memory->write_bytes(0x3010, reinterpret_cast<const u8*>(&table), 8));
    check_parity({0x45, 0x8B, 0xF9,              // mov r15d,r9d
                  0x4C, 0x8B, 0xE9,              // mov r13,rcx
                  0x49, 0xC1, 0xE7, 0x04,        // shl r15,4
                  0x48, 0x8B, 0xEA,              // mov rbp,rdx
                  0x41, 0x8B, 0xDB,              // mov ebx,r11d
                  0x4D, 0x03, 0x78, 0x10},       // add r15,[r8+0x10]
        [](CpuState& cpu) {
            cpu.gpr.set64(R15, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(R9, 0xDEADBEEF00000003ull);
            cpu.gpr.set64(R8, 0x3000);
            cpu.gpr.set64(R11, 0x1111111122222222ull);
        });
}

TEST_F(JitParity64Test, BswapR64) {
    // BSWAP RAX — 48 0F C8
    check_parity({0x48, 0x0F, 0xC8},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x0123456789ABCDEFull); },
        false);
}

TEST_F(JitParity64Test, TzcntR64PreservesFullWidth) {
    check_parity({0xF3, 0x48, 0x0F, 0xBC, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RCX, 1ull << 40);
            cpu.gpr.set64(RAX, 0xAAAAAAAAAAAAAAAAull);
        });
}

TEST_F(JitParity64Test, LzcntR64ZeroReturns64) {
    check_parity({0xF3, 0x48, 0x0F, 0xBD, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RCX, 0);
            cpu.gpr.set64(RAX, 0xAAAAAAAAAAAAAAAAull);
        });
}

TEST_F(JitParity64Test, BswapR8) {
    // BSWAP R8 — 49 0F C8 (REX.B sets parent reg = R8)
    check_parity({0x49, 0x0F, 0xC8},
        [](CpuState& cpu) { cpu.gpr.set64(R8, 0xFEEDFACECAFEBEEFull); },
        false);
}

TEST_F(JitParity64Test, CmovzR64Taken) {
    // XOR RAX, RAX (sets ZF); CMOVZ RCX, RDX
    u8 code[] = {
        0x48, 0x31, 0xC0,              // XOR RAX, RAX → ZF=1
        0x48, 0x0F, 0x44, 0xCA,        // CMOVZ RCX, RDX
    };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            cpu.gpr.set64(RCX, 0x1111111111111111ull);
            cpu.gpr.set64(RDX, 0xDEADBEEFCAFEBABEull);
        });
}

TEST_F(JitParity64Test, CmovzR64NotTaken) {
    // MOV RAX, 1 (clears ZF); CMOVZ RCX, RDX → unchanged
    u8 code[] = {
        0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00,  // MOV RAX, 1
        0x48, 0x85, 0xC0,                              // TEST RAX, RAX → ZF=0
        0x48, 0x0F, 0x44, 0xCA,                        // CMOVZ RCX, RDX
    };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            cpu.gpr.set64(RCX, 0x1111111111111111ull);
            cpu.gpr.set64(RDX, 0xDEADBEEFCAFEBABEull);
        });
}

TEST_F(JitParity64Test, CmovnzR64Taken) {
    // MOV RAX, 1; TEST RAX,RAX (clears ZF); CMOVNZ RCX, RDX
    u8 code[] = {
        0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00,
        0x48, 0x85, 0xC0,
        0x48, 0x0F, 0x45, 0xCA,        // CMOVNZ RCX, RDX
    };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            cpu.gpr.set64(RCX, 0x1111111111111111ull);
            cpu.gpr.set64(RDX, 0xDEADBEEFCAFEBABEull);
        });
}

// ---------------------------------------------------------------------------
// CMOVcc with a MEMORY source. The r/m form has to read guest memory, and that
// read both clobbers NZCV (bounds-check SUBS) and, on a region-cache miss,
// calls the read helper with X0 = JitContext* and returns in X0. A host
// condition captured into W0 before the load is therefore destroyed on exactly
// the slow path, which flips the select. Intel SDM Vol. 2 CMOVcc: the source is
// read but the condition is the one live at the instruction, and EFLAGS are
// unaffected.
//
// This is the MSVC small-string-optimization shape that faulted Steam's
// bootstrapper: CMP R15, 0x10 / CMOVAE RAX, [RBP+0x30] picks the heap pointer
// only when capacity >= 16, and taking it at capacity 15 hands the following
// store a buffer's worth of characters as its base address.
TEST_F(JitParity64Test, CmovaeR64MemSourceNotTaken) {
    ASSERT_TRUE(m_memory->write_u64(0x2030, 0x4141414141414165ull).is_ok());
    u8 code[] = {
        0x49, 0x83, 0xFF, 0x10,              // CMP R15, 0x10  → CF=1 (below)
        0x48, 0x0F, 0x43, 0x45, 0x30,        // CMOVAE RAX, [RBP+0x30]
    };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            cpu.gpr.set64(R15, 0x0F);
            cpu.gpr.set64(RBP, 0x2000);
            cpu.gpr.set64(RAX, 0x1111111111111111ull);
        });
}

TEST_F(JitParity64Test, CmovaeR64MemSourceTaken) {
    ASSERT_TRUE(m_memory->write_u64(0x2030, 0x4141414141414165ull).is_ok());
    u8 code[] = {
        0x49, 0x83, 0xFF, 0x10,              // CMP R15, 0x10  → CF=0 (>=)
        0x48, 0x0F, 0x43, 0x45, 0x30,        // CMOVAE RAX, [RBP+0x30]
    };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            cpu.gpr.set64(R15, 0x20);
            cpu.gpr.set64(RBP, 0x2000);
            cpu.gpr.set64(RAX, 0x1111111111111111ull);
        });
}

TEST_F(JitParity64Test, CmovaeR32MemSourceNotTaken) {
    // Same hazard for the 32-bit form inside a 64-bit block; a not-taken
    // CMOVcc r32 still zero-extends DEST[63:32] (SDM Vol. 2).
    ASSERT_TRUE(m_memory->write_u64(0x2030, 0x4141414141414165ull).is_ok());
    u8 code[] = {
        0x41, 0x83, 0xFF, 0x10,              // CMP R15D, 0x10 → CF=1
        0x0F, 0x43, 0x45, 0x30,              // CMOVAE EAX, [RBP+0x30]
    };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            cpu.gpr.set64(R15, 0x0F);
            cpu.gpr.set64(RBP, 0x2000);
            cpu.gpr.set64(RAX, 0x1111111111111111ull);
        });
}

TEST_F(JitParity64Test, CmovzR64MemSourceNotTakenAfterTest) {
    // Same select driven by a logical flag producer (TEST → And) rather than
    // CMP, so the fused-condition mapping is exercised on a second NZCV kind.
    ASSERT_TRUE(m_memory->write_u64(0x2030, 0x4141414141414165ull).is_ok());
    u8 code[] = {
        0x48, 0xC7, 0xC3, 0x01, 0x00, 0x00, 0x00,  // MOV RBX, 1
        0x48, 0x85, 0xDB,                          // TEST RBX, RBX → ZF=0
        0x48, 0x0F, 0x44, 0x45, 0x30,              // CMOVZ RAX, [RBP+0x30]
    };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            cpu.gpr.set64(RBP, 0x2000);
            cpu.gpr.set64(RAX, 0x1111111111111111ull);
        });
}

TEST_F(JitParity64Test, CmovlR64) {
    // CMP RAX, RBX (signed less); CMOVL RCX, RDX
    u8 code[] = {
        0x48, 0x39, 0xD8,              // CMP RAX, RBX
        0x48, 0x0F, 0x4C, 0xCA,        // CMOVL RCX, RDX
    };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, static_cast<u64>(-5));
            cpu.gpr.set64(RBX, 10);
            cpu.gpr.set64(RCX, 0x1111111111111111ull);
            cpu.gpr.set64(RDX, 0xDEADBEEFCAFEBABEull);
        });
}

TEST_F(JitParity64Test, PushRm64Reg) {
    // PUSH RAX via FF /6 (FF F0) — uses RM64 path
    check_parity({0xFF, 0xF0, 0x58},  // PUSH RAX (RM64); POP RAX
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0xAABBCCDDEEFF0011ull); },
        false);
}

TEST_F(JitParity64Test, PopRm64Reg) {
    // PUSH RBX; POP RCX via 8F /0 (8F C1) — uses RM64 path
    check_parity({0x53, 0x8F, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RBX, 0xFEDCBA9876543210ull);
            cpu.gpr.set64(RCX, 0);
        },
        false);
}

// ============================================================================
// PUSH immediate (64-bit sign-extended)
// ============================================================================

TEST_F(JitParity64Test, PushImm8) {
    // PUSH 5 (6A 05); POP RAX — sign-extended i8 to 64 bits
    check_parity({0x6A, 0x05, 0x58}, {}, false);
}

TEST_F(JitParity64Test, PushImm8Negative) {
    // PUSH -1 (6A FF); POP RAX — 0xFFFFFFFFFFFFFFFF
    check_parity({0x6A, 0xFF, 0x58}, {}, false);
}

TEST_F(JitParity64Test, PushImm32) {
    // PUSH 0x1000 (68 00 10 00 00); POP RAX
    check_parity({0x68, 0x00, 0x10, 0x00, 0x00, 0x58}, {}, false);
}

TEST_F(JitParity64Test, PushImm32Negative) {
    // PUSH -0x1000 (68 00 F0 FF FF); POP RAX — sign-extended to 0xFFFFFFFFFFFFF000
    check_parity({0x68, 0x00, 0xF0, 0xFF, 0xFF, 0x58}, {}, false);
}

// ============================================================================
// 64-bit shifts — by-1 form
// ============================================================================

TEST_F(JitParity64Test, ShlR64By1) {
    // SHL RAX, 1 — 48 D1 E0
    check_parity({0x48, 0xD1, 0xE0},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x0000000100000000ull); });
}

TEST_F(JitParity64Test, ShrR64By1) {
    // SHR RAX, 1 — 48 D1 E8 (logical; zero fills sign bit)
    check_parity({0x48, 0xD1, 0xE8},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x8000000000000001ull); });
}

TEST_F(JitParity64Test, SarR64By1) {
    // SAR RAX, 1 — 48 D1 F8 (arithmetic; sign bit preserved)
    check_parity({0x48, 0xD1, 0xF8},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, static_cast<u64>(-8)); });
}

// ============================================================================
// 64-bit shifts — CL form
// ============================================================================

TEST_F(JitParity64Test, ShlR64ByCL) {
    // SHL RAX, CL — 48 D3 E0; CL=32
    check_parity({0x48, 0xD3, 0xE0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 1);
            cpu.gpr.set64(RCX, 32);
        });
}

TEST_F(JitParity64Test, ShlR64ByCLZero) {
    // SHL RAX, CL with CL=0 — flags must NOT change
    check_parity({0x48, 0xD3, 0xE0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x12345678ABCDEFull);
            cpu.gpr.set64(RCX, 0);
        });
}

TEST_F(JitParity64Test, ShrR64ByCL) {
    // SHR RAX, CL — 48 D3 E8; CL=16
    check_parity({0x48, 0xD3, 0xE8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x100000000ull);
            cpu.gpr.set64(RCX, 16);
        });
}

TEST_F(JitParity64Test, SarR64ByCL) {
    // SAR RAX, CL — 48 D3 F8; CL=4 with negative value
    check_parity({0x48, 0xD3, 0xF8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, static_cast<u64>(-16384));
            cpu.gpr.set64(RCX, 4);
        });
}

// ============================================================================
// IMUL three-operand
// ============================================================================

TEST_F(JitParity64Test, ImulR64R64Imm8) {
    // IMUL RAX, RBX, 7 — 48 6B C3 07
    check_parity({0x48, 0x6B, 0xC3, 0x07},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0);
            cpu.gpr.set64(RBX, 6);
        });
}

TEST_F(JitParity64Test, ImulR64R64Imm32) {
    // IMUL RAX, RBX, 0x100 — 48 69 C3 00 01 00 00
    check_parity({0x48, 0x69, 0xC3, 0x00, 0x01, 0x00, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0);
            cpu.gpr.set64(RBX, 0x8000000ull);
        });
}

TEST_F(JitParity64Test, ImulR64Overflow) {
    // IMUL RAX, RBX, 2 — result overflows signed 64-bit; CF=OF=1
    // 48 6B C3 02
    check_parity({0x48, 0x6B, 0xC3, 0x02},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0);
            cpu.gpr.set64(RBX, 0x7FFFFFFFFFFFFFFFull);
        });
}

// ============================================================================
// TEST r/m64, imm32
// ============================================================================

TEST_F(JitParity64Test, TestRm64Imm32NonZero) {
    // TEST RAX, 0xFF — 48 F7 C0 FF 00 00 00; RAX has bit 0-7 set
    check_parity({0x48, 0xF7, 0xC0, 0xFF, 0x00, 0x00, 0x00},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0xFFull); });
}

TEST_F(JitParity64Test, TestRm64Imm32Zero) {
    // TEST RAX, 0x1 — result is 0 since RAX has no low bits set
    check_parity({0x48, 0xF7, 0xC0, 0x01, 0x00, 0x00, 0x00},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x100000000ull); });
}

// ============================================================================
// SETcc r/m8 (register dest, indices 0-3 only in JIT)
// ============================================================================

TEST_F(JitParity64Test, SetzAlTaken) {
    // CMP RAX, RBX (equal → ZF=1); SETZ AL → AL=1
    // CMP: 48 39 D8; SETZ AL: 0F 94 C0
    check_parity({0x48, 0x39, 0xD8, 0x0F, 0x94, 0xC0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 42);
            cpu.gpr.set64(RBX, 42);
        },
        false);
}

TEST_F(JitParity64Test, SetzAlNotTaken) {
    // CMP RAX, RBX (not equal → ZF=0); SETZ AL → AL=0
    check_parity({0x48, 0x39, 0xD8, 0x0F, 0x94, 0xC0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 42);
            cpu.gpr.set64(RBX, 99);
        },
        false);
}

TEST_F(JitParity64Test, SetnzCl) {
    // TEST RAX, RAX (nonzero → ZF=0); SETNZ CL → CL=1
    // TEST RAX, RAX: 48 85 C0; SETNZ CL: 0F 95 C1
    check_parity({0x48, 0x85, 0xC0, 0x0F, 0x95, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 1);
            cpu.gpr.set64(RCX, 0);
        },
        false);
}

// ============================================================================
// 64-bit ROL / ROR
// ============================================================================

TEST_F(JitParity64Test, RolR64By1) {
    // ROL RAX, 1 — 48 D1 C0; MSB=1 → CF=1, OF = MSB(result) XOR CF
    check_parity({0x48, 0xD1, 0xC0},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x8000000000000001ull); });
}

TEST_F(JitParity64Test, RolR64Imm8) {
    // ROL RAX, 4 — 48 C1 C0 04
    check_parity({0x48, 0xC1, 0xC0, 0x04},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0xF000000000000001ull); });
}

TEST_F(JitParity64Test, RolR64ByCL) {
    // ROL RAX, CL — 48 D3 C0; CL=20
    check_parity({0x48, 0xD3, 0xC0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xDEADBEEF12345678ull);
            cpu.gpr.set64(RCX, 20);
        });
}

TEST_F(JitParity64Test, RolR64ByCLZero) {
    // ROL RAX, CL with CL=0 — flags must NOT change
    check_parity({0x48, 0xD3, 0xC0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x1234567812345678ull);
            cpu.gpr.set64(RCX, 0);
        });
}

TEST_F(JitParity64Test, RorR64By1) {
    // ROR RAX, 1 — 48 D1 C8; LSB=1 → bit 63 of result = 1
    check_parity({0x48, 0xD1, 0xC8},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x0000000000000001ull); });
}

TEST_F(JitParity64Test, RorR64Imm8) {
    // ROR RAX, 8 — 48 C1 C8 08
    check_parity({0x48, 0xC1, 0xC8, 0x08},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x00000000000000FFull); });
}

TEST_F(JitParity64Test, RorR64ByCL) {
    // ROR RAX, CL — 48 D3 C8; CL=12
    check_parity({0x48, 0xD3, 0xC8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x123456789ABCDEF0ull);
            cpu.gpr.set64(RCX, 12);
        });
}

TEST_F(JitParity64Test, RorR64ByCLZero) {
    // ROR RAX, CL with CL=0 — flags must NOT change
    check_parity({0x48, 0xD3, 0xC8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xCAFEBABECAFEBABEull);
            cpu.gpr.set64(RCX, 0);
        });
}
