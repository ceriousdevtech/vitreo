// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): LEA, MUL/IMUL and DIV/IDIV
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

// ============================================================================
// LEA parity (no flags affected)
// ============================================================================

TEST_F(JitParityTest, LeaBaseDisp) {
    check_parity({0x8D, 0x43, 0x10}, // LEA EAX, [EBX+0x10]
        [](CpuState& cpu) { cpu.gpr.set32(EBX, 0x1000); },
        false);
}

TEST_F(JitParityTest, LeaBaseIndexScale) {
    // LEA EAX, [EBX+ECX*4+0x10] — 8D 44 8B 10
    check_parity({0x8D, 0x44, 0x8B, 0x10},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x1000);
            cpu.gpr.set32(ECX, 0x10);
        },
        false);
}

TEST_F(JitParityTest, LeaRegPlusReg) {
    // LEA EAX, [EBX+ECX] — 8D 04 0B
    // ModRM 04: mod=00, reg=000(EAX), rm=100(SIB)
    // SIB 0B: scale=00(×1), index=001(ECX), base=011(EBX)
    check_parity({0x8D, 0x04, 0x0B},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x2000);
            cpu.gpr.set32(ECX, 0x100);
        },
        false);
}

TEST_F(JitParityTest, LeaBaseOnlyEcx) {
    // LEA EAX, [ECX] — 8D 01
    check_parity({0x8D, 0x01},
        [](CpuState& cpu) {
            cpu.gpr.set32(ECX, 0x12345678);
        },
        false);
}

TEST_F(JitParityTest, LeaBaseOnlyEbx) {
    // LEA EAX, [EBX] — 8D 03
    check_parity({0x8D, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x87654321);
        },
        false);
}

TEST_F(JitParityTest, LeaBaseOnlyEspSib) {
    // LEA EAX, [ESP] — 8D 04 24 (SIB required for ESP base)
    check_parity({0x8D, 0x04, 0x24},
        [](CpuState& cpu) {
            cpu.gpr.set32(ESP, 0x0012FF00);
        },
        false);
}

// ============================================================================
// IMUL parity
// ============================================================================

TEST_F(JitParityTest, ImulRegReg) {
    // IMUL EAX, EBX — 0F AF C3
    check_parity({0x0F, 0xAF, 0xC3},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 7);
            cpu.gpr.set32(EBX, 6);
        });
}

TEST_F(JitParityTest, ImulOverflow) {
    // ZF/SF are undefined after IMUL, only CF/OF are defined
    check_parity({0x0F, 0xAF, 0xC3},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x10000);
            cpu.gpr.set32(EBX, 0x10000);
        }, /* check_flags */ false);
}

TEST_F(JitParityTest, ImulNegative) {
    // ZF/SF are undefined after IMUL, only CF/OF are defined
    check_parity({0x0F, 0xAF, 0xC3},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, static_cast<u32>(-3));
            cpu.gpr.set32(EBX, 7);
        }, /* check_flags */ false);
}

TEST_F(JitParityTest, Imul16RegRegPreservesUpperHalfAndFlags) {
    // IMUL AX, BX — 66 0F AF C3. 300 * 300 overflows signed 16 bits, while the
    // upper half of EAX must survive the 16-bit destination write.
    check_parity({0x66, 0x0F, 0xAF, 0xC3},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xA5A5012Cu);
            cpu.gpr.set32(EBX, 0x0000012Cu);
        });
}

TEST_F(JitParityTest, Imul16MemorySource) {
    ASSERT_TRUE(m_memory->write_u16(0x2000, 0xFFFD).is_ok()); // -3
    // IMUL SI, word ptr [0x2000] — 66 0F AF 35 00 20 00 00
    check_parity({0x66, 0x0F, 0xAF, 0x35, 0x00, 0x20, 0x00, 0x00},
        [](CpuState& cpu) { cpu.gpr.set32(ESI, 0xBEEF0007u); });
}

TEST_F(JitParityTest, Imul16ImmediateForms) {
    // IMUL AX, BX, -5; IMUL CX, DX, 0x100.
    check_parity({0x66, 0x6B, 0xC3, 0xFB,
                  0x66, 0x69, 0xCA, 0x00, 0x01},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 7);
            cpu.gpr.set32(EDX, 0x80);
        });
}

TEST_F(JitParityTest, MulRm8Reg) {
    // MUL CL — F6 E1. ZF/SF/PF/AF undefined after MUL.
    check_parity({0xF6, 0xE1},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xA5A500C8u); // AL=200
            cpu.gpr.set32(ECX, 2);
        }, /*check_flags=*/false);
}

TEST_F(JitParityTest, ImulRm8Reg) {
    // IMUL CL — F6 E9. CF/OF defined; other flags undefined.
    check_parity({0xF6, 0xE9},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xBEEF007Fu); // AL=127
            cpu.gpr.set32(ECX, 2);           // 254 does not fit in i8
        }, /*check_flags=*/false);
}

TEST_F(JitParityTest, DivRm8Reg) {
    check_parity({0xF6, 0xF1}, // DIV CL
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 100);
            cpu.gpr.set32(ECX, 7);
        }, /*check_flags=*/false);
}

TEST_F(JitParityTest, IdivRm8Reg) {
    check_parity({0xF6, 0xF9}, // IDIV CL
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, static_cast<u16>(static_cast<i16>(-20)));
            cpu.gpr.set32(ECX, 3);
        }, /*check_flags=*/false);
}

TEST_F(JitParityTest, DivRm8Memory) {
    ASSERT_TRUE(m_memory->write_u8(0x2000, 5).is_ok());
    check_parity({0xF6, 0x35, 0x00, 0x20, 0x00, 0x00}, // div byte ptr [0x2000]
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 47); },
        /*check_flags=*/false);
}

TEST_F(JitParityTest, MulRm16Reg) {
    // MUL CX — 66 F7 E1. DX:AX := AX * CX; CF/OF defined, others undefined.
    check_parity({0x66, 0xF7, 0xE1},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xA5A5012Cu); // AX=300
            cpu.gpr.set32(ECX, 0x0000012Cu); // CX=300 → product 90000
            cpu.gpr.set32(EDX, 0xDEADBEEFu);
        }, /*check_flags=*/false);
}

TEST_F(JitParityTest, ImulRm16Reg) {
    // IMUL CX — 66 F7 E9. 200 * 200 overflows signed 16-bit product half.
    check_parity({0x66, 0xF7, 0xE9},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xBEEF00C8u); // AX=200
            cpu.gpr.set32(ECX, 0x000000C8u); // CX=200
            cpu.gpr.set32(EDX, 0x11112222u);
        }, /*check_flags=*/false);
}

TEST_F(JitParityTest, DivRm16Reg) {
    // DIV CX — 66 F7 F1. DX:AX=1000 / CX=7 → AX=142 rem DX=6
    check_parity({0x66, 0xF7, 0xF1},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xABCD03E8u); // AX=1000
            cpu.gpr.set32(EDX, 0xDCBA0000u); // DX=0
            cpu.gpr.set32(ECX, 7);
        }, /*check_flags=*/false);
}

TEST_F(JitParityTest, IdivRm16Reg) {
    // IDIV CX — 66 F7 F9. DX:AX=-1000 / CX=7 → AX=-142 rem DX=-6
    check_parity({0x66, 0xF7, 0xF9},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xABCDFC18u); // AX=-1000
            cpu.gpr.set32(EDX, 0xDCBAFFFFu); // DX=0xFFFF
            cpu.gpr.set32(ECX, 7);
        }, /*check_flags=*/false);
}

TEST_F(JitParityTest, DivRm16Memory) {
    ASSERT_TRUE(m_memory->write_u16(0x2000, 5).is_ok());
    // div word ptr [0x2000] — 66 F7 35 00 20 00 00
    check_parity({0x66, 0xF7, 0x35, 0x00, 0x20, 0x00, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x1234002Fu); // AX=47
            cpu.gpr.set32(EDX, 0x56780000u); // DX=0
        }, /*check_flags=*/false);
}



// ===========================================================================
// DIV/IDIV with a base+index+displacement memory operand.
//
// Regression for a JIT/interpreter divergence found in retail StarCraft: the
// DIV/IDIV codegen asked emit_compute_ea to place the effective address in
// REG_EA_TMP, which is that helper's *own* internal scratch. The base was
// therefore overwritten by the index before being added, so
// `div [esp + esi*4 + 0x3c]` computed esi*4 + esi*4 + 0x3c instead of
// esp + esi*4 + 0x3c. With esi=2 that produced address 0x4c, and the bogus
// divisor yielded a bogus remainder that the game used as a table index.
// ===========================================================================

TEST_F(JitParityTest, DivMemSibBaseIndexDisp) {
    // F7 /6 with mod=01, rm=100 (SIB), scale=4, index=ESI, base=ESP, disp8=0x3c
    u8 code[] = { 0xF7, 0x74, 0xB4, 0x3C };  // div dword ptr [esp+esi*4+0x3c]
    auto init = [this](CpuState& cpu) {
        cpu.gpr.set32(ESI, 2);
        cpu.gpr.set32(EAX, 1000);
        cpu.gpr.set32(EDX, 0);
        // Divisor at the correct EA: 0x10000 + 2*4 + 0x3c = 0x10044.
        (void)m_memory->write_u32(0x10044, 7);
        // A different value at the address the buggy codegen computed
        // (esi*8 + 0x3c = 0x4c) would be unmapped here, so a regression shows
        // up as an execution failure or a diverged quotient/remainder.
    };
    check_parity(code, sizeof(code), init, /*check_flags=*/false);
}

TEST_F(JitParityTest, IdivMemSibBaseIndexDisp) {
    // F7 /7 — same addressing form, signed divide.
    u8 code[] = { 0xF7, 0x7C, 0xB4, 0x3C };  // idiv dword ptr [esp+esi*4+0x3c]
    auto init = [this](CpuState& cpu) {
        cpu.gpr.set32(ESI, 2);
        cpu.gpr.set32(EAX, 1000);
        cpu.gpr.set32(EDX, 0);
        (void)m_memory->write_u32(0x10044, 7);
    };
    check_parity(code, sizeof(code), init, /*check_flags=*/false);
}

TEST_F(JitParityTest, DivMemSibAfterPushAdjustsEsp) {
    // The exact retail shape: a PUSH immediately before the DIV, so the
    // divisor's address depends on the post-push ESP.
    u8 code[] = {
        0x6A, 0x00,              // push 0
        0xF7, 0x74, 0xB4, 0x3C,  // div dword ptr [esp+esi*4+0x3c]
    };
    auto init = [this](CpuState& cpu) {
        cpu.gpr.set32(ESI, 2);
        cpu.gpr.set32(EAX, 0x3EE);
        cpu.gpr.set32(EDX, 0);
        // After `push 0`, esp = 0xFFFC, so EA = 0xFFFC + 8 + 0x3c = 0x10040.
        (void)m_memory->write_u32(0x10040, 13);
    };
    check_parity(code, sizeof(code), init, /*check_flags=*/false);
}

TEST_F(JitParityTest, DivMemSibScale8LargeDisp) {
    // Scale 8 with a displacement too large for an ARM64 add-immediate, which
    // takes emit_compute_ea's materialise-into-scratch path.
    u8 code[] = { 0xF7, 0xB4, 0xF3, 0x00, 0x10, 0x00, 0x00 };
    auto init = [this](CpuState& cpu) {
        cpu.gpr.set32(EBX, 0x2000);
        cpu.gpr.set32(ESI, 1);
        cpu.gpr.set32(EAX, 500);
        cpu.gpr.set32(EDX, 0);
        (void)m_memory->write_u32(0x2000 + 8 + 0x1000, 3);
    };
    check_parity(code, sizeof(code), init, /*check_flags=*/false);
}
