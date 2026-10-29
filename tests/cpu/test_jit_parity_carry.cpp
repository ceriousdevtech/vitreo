// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): carry-only lazy-flag capture
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

// ============================================================================
// Carry-only lazy-flag capture
// ============================================================================

TEST_F(JitParityTest, AddThenAdcCapturesCarry) {
    // ADD EAX,EBX overflows unsigned; ADC ECX,EDX must consume CF=1.
    check_parity({0x01, 0xD8, 0x11, 0xD1},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xFFFFFFFFu);
            cpu.gpr.set32(EBX, 1);
            cpu.gpr.set32(ECX, 10);
            cpu.gpr.set32(EDX, 5);
        });
}

TEST_F(JitParityTest, Add8ThenAdcCapturesCarry) {
    // ADD AL,BL wraps at 8 bits; ADC ECX,EDX must consume CF=1.
    check_parity({0x00, 0xD8, 0x11, 0xD1},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x000000FFu);
            cpu.gpr.set32(EBX, 1);
            cpu.gpr.set32(ECX, 10);
            cpu.gpr.set32(EDX, 5);
        });
}

TEST_F(JitParityTest, AddMovAdcCapturesCarry) {
    // The OpenSSL Comba multiply-accumulate idiom: a register-to-register MOV
    // sits between the ADD carry producer and the ADC consumer
    // (add ecx,eax; mov edi,edx; adc esi,edi). The reg-reg MOV is NZCV-neutral
    // in codegen, so the JIT's ADDS carry must survive it and feed the ADC.
    //   01 C1  add ecx,eax
    //   89 D7  mov edi,edx
    //   11 FE  adc esi,edi
    check_parity({0x01, 0xC1, 0x89, 0xD7, 0x11, 0xFE},
        [](CpuState& cpu) {
            cpu.gpr.set32(ECX, 0xFFFFFFFFu);  // ADD ECX,EAX overflows → CF=1
            cpu.gpr.set32(EAX, 1);
            cpu.gpr.set32(EDX, 7);            // MOV EDI,EDX
            cpu.gpr.set32(ESI, 100);          // ADC ESI,EDI consumes CF=1
        });
}

TEST_F(JitParityTest, AddMovAdcNoCarry) {
    // Same shape, but the ADD does not overflow → CF=0 must also survive the MOV.
    check_parity({0x01, 0xC1, 0x89, 0xD7, 0x11, 0xFE},
        [](CpuState& cpu) {
            cpu.gpr.set32(ECX, 1);           // ADD ECX,EAX → no overflow, CF=0
            cpu.gpr.set32(EAX, 2);
            cpu.gpr.set32(EDX, 7);
            cpu.gpr.set32(ESI, 100);
        });
}

TEST_F(JitParityTest, AddMovChainAdcCapturesCarry) {
    // Two chained reg-reg MOVs between ADD and ADC: NZCV must survive both.
    //   01 C1  add ecx,eax
    //   89 D7  mov edi,edx
    //   89 FB  mov ebx,edi
    //   11 DE  adc esi,ebx
    check_parity({0x01, 0xC1, 0x89, 0xD7, 0x89, 0xFB, 0x11, 0xDE},
        [](CpuState& cpu) {
            cpu.gpr.set32(ECX, 0xFFFFFFFFu);
            cpu.gpr.set32(EAX, 5);
            cpu.gpr.set32(EDX, 9);
            cpu.gpr.set32(ESI, 100);
        });
}

TEST_F(JitParityTest, AddMovImmAdcCapturesCarry) {
    // A reg,imm MOV (also NZCV-neutral) between ADD and ADC.
    //   01 C1        add ecx,eax
    //   BF 07 00 00 00  mov edi,7
    //   11 FE        adc esi,edi
    check_parity({0x01, 0xC1, 0xBF, 0x07, 0x00, 0x00, 0x00, 0x11, 0xFE},
        [](CpuState& cpu) {
            cpu.gpr.set32(ECX, 0xFFFFFFFFu);
            cpu.gpr.set32(EAX, 1);
            cpu.gpr.set32(ESI, 100);
        });
}

// (Jcc fusion after ADD — including the unsigned B/NB conditions that map
// oppositely to SUB — is validated against the interpreter for all 16
// conditions in JitExecTest.AddThenFusedJccMatchesInterpreterForAllConditions,
// which uses an execution harness that models the forward branch correctly.)

TEST_F(JitParityTest, SubThenSbbCapturesBorrow) {
    // SUB EAX,EBX borrows; SBB ECX,EDX must consume CF=1.
    check_parity({0x29, 0xD8, 0x19, 0xD1},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 3);
            cpu.gpr.set32(EBX, 5);
            cpu.gpr.set32(ECX, 10);
            cpu.gpr.set32(EDX, 5);
        });
}

TEST_F(JitParityTest, CmpThenSbbSelfMaskSelectsPointer) {
    // cmp ecx,esi; sbb eax,eax; and eax,edx — branchless "i < n ? p : 0".
    // ecx=0 < esi=3 borrows, so EAX must end up equal to EDX.
    check_parity({0x3B, 0xCE, 0x1B, 0xC0, 0x23, 0xC2},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x12345678);
            cpu.gpr.set32(ECX, 0);
            cpu.gpr.set32(ESI, 3);
            cpu.gpr.set32(EDX, 0x2468);
        });
}

TEST_F(JitParityTest, D3DCompilerCountLoopHeaderSelectsPointer) {
    // d3dcompiler_47 loop header, side exits not taken: sete/dec/and-mem
    // between a CMP and the CMP;SBB;AND pointer select. EAX must equal EDX.
    const u32 ebp = 0x3000;
    const u32 count = 3, table = 0x2468;
    ASSERT_TRUE(m_memory->write_bytes(ebp - 0xa4, reinterpret_cast<const u8*>(&count), 4));
    ASSERT_TRUE(m_memory->write_bytes(ebp - 0xac, reinterpret_cast<const u8*>(&table), 4));
    const u8 one = 1;
    ASSERT_TRUE(m_memory->write_bytes(ebp - 0xad, &one, 1));
    check_parity({0x8B, 0xF0,                          // mov esi,eax
                  0x85, 0xF6,                          // test esi,esi
                  0x0F, 0x88, 0x0A, 0x02, 0x00, 0x00,  // js (not taken)
                  0x81, 0xFE, 0x02, 0x00, 0x0F, 0x27,  // cmp esi,0x270f0002
                  0x8B, 0xB5, 0x5C, 0xFF, 0xFF, 0xFF,  // mov esi,[ebp-0xa4]
                  0x0F, 0x94, 0xC0,                    // sete al
                  0x33, 0xFF,                          // xor edi,edi
                  0xFE, 0xC8,                          // dec al
                  0x33, 0xC9,                          // xor ecx,ecx
                  0x20, 0x85, 0x53, 0xFF, 0xFF, 0xFF,  // and [ebp-0xad],al
                  0x85, 0xF6,                          // test esi,esi
                  0x0F, 0x84, 0xE5, 0x01, 0x00, 0x00,  // je (not taken)
                  0x8B, 0x95, 0x54, 0xFF, 0xFF, 0xFF,  // mov edx,[ebp-0xac]
                  0x3B, 0xCE,                          // cmp ecx,esi
                  0x1B, 0xC0,                          // sbb eax,eax
                  0x23, 0xC2},                         // and eax,edx
        [&](CpuState& cpu) {
            cpu.gpr.set32(EAX, 5);
            cpu.gpr.set32(EBP, ebp);
        });
}

TEST_F(JitParityTest, Sub16ThenSbbCapturesBorrow) {
    // SUB AX,BX borrows at 16 bits; SBB ECX,EDX must consume CF=1.
    check_parity({0x66, 0x29, 0xD8, 0x19, 0xD1},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 3);
            cpu.gpr.set32(EBX, 5);
            cpu.gpr.set32(ECX, 10);
            cpu.gpr.set32(EDX, 5);
        });
}

TEST_F(JitParityTest, LogicThenAdcCapturesClearedCarry) {
    // XOR clears CF even when the previously materialized CF was set.
    check_parity({0x31, 0xC0, 0x11, 0xD1},
        [](CpuState& cpu) {
            cpu.flags.set_cf(true);
            cpu.gpr.set32(EAX, 0x12345678u);
            cpu.gpr.set32(ECX, 10);
            cpu.gpr.set32(EDX, 5);
        });
}

TEST_F(JitParityTest, Logic16ThenAdcCapturesClearedCarry) {
    // XOR AX,AX clears CF without materializing unrelated arithmetic flags.
    check_parity({0x66, 0x31, 0xC0, 0x11, 0xD1},
        [](CpuState& cpu) {
            cpu.flags.set_cf(true);
            cpu.gpr.set32(EAX, 0x12345678u);
            cpu.gpr.set32(ECX, 10);
            cpu.gpr.set32(EDX, 5);
        });
}

TEST_F(JitParityTest, ShiftThenAdcCapturesShiftedOutBit) {
    // SHL shifts bit 31 into CF; ADC must consume it.
    check_parity({0xD1, 0xE0, 0x11, 0xD1},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x80000000u);
            cpu.gpr.set32(ECX, 10);
            cpu.gpr.set32(EDX, 5);
        });
}

TEST_F(JitParityTest, IncPreservesPendingCompareCarry) {
    // CMP sets CF=1; INC must preserve it; ADC then consumes it.
    check_parity({0x39, 0xD8, 0x41, 0x11, 0xD2},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 3);
            cpu.gpr.set32(EBX, 5);
            cpu.gpr.set32(ECX, 7);
            cpu.gpr.set32(EDX, 10);
        });
}

TEST_F(JitParityTest, ChainedAdcCapturesPreviousAdcCarry) {
    // First ADC wraps and sets CF; the second ADC must consume that lazy CF.
    check_parity({0x11, 0xD1, 0x11, 0xD1},
        [](CpuState& cpu) {
            cpu.flags.set_cf(true);
            cpu.gpr.set32(ECX, 0xFFFFFFFFu);
            cpu.gpr.set32(EDX, 0);
        });
}

TEST_F(JitParityTest, ChainedSbbCapturesPreviousSbbBorrow) {
    // First SBB borrows; the second SBB must consume that lazy CF.
    check_parity({0x19, 0xD1, 0x19, 0xD1},
        [](CpuState& cpu) {
            cpu.flags.set_cf(true);
            cpu.gpr.set32(ECX, 0);
            cpu.gpr.set32(EDX, 0);
        });
}

TEST_F(JitParityTest, MulThenAdcCapturesSetCarry) {
    // MUL EBX produces a nonzero high half; ADC ECX,0 must consume CF=1.
    check_parity({0xF7, 0xE3, 0x83, 0xD1, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xFFFFFFFFu);
            cpu.gpr.set32(EBX, 2);
            cpu.gpr.set32(ECX, 10);
        });
}

TEST_F(JitParityTest, MulThenAdcCapturesClearCarry) {
    // MUL EBX fits in EAX; ADC ECX,0 must consume CF=0.
    check_parity({0xF7, 0xE3, 0x83, 0xD1, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 3);
            cpu.gpr.set32(EBX, 7);
            cpu.gpr.set32(ECX, 10);
        });
}

TEST_F(JitParityTest, SbbZeroImmediatePreservesBorrowContract) {
    for (bool carry : {false, true}) {
        for (u32 value : {0u, 1u, 0x80000000u, 0xFFFFFFFFu}) {
            check_parity({0x83, 0xD9, 0x00},  // sbb ecx,0
                [=](CpuState& cpu) {
                    cpu.flags.set_cf(carry);
                    cpu.gpr.set32(ECX, value);
                });
        }
    }
}

TEST_F(JitParityTest, AdcSbbZeroImmediatePreservesAuxCarryAndMemoryForms) {
    const struct {
        std::vector<u8> code;
        u8 base_reg;
    } cases[] = {
        {{0x83, 0xD1, 0x00}, ECX},                    // adc ecx,0
        {{0x81, 0xD1, 0, 0, 0, 0}, ECX},             // adc ecx,imm32(0)
        {{0x83, 0xD9, 0x00}, ECX},                    // sbb ecx,0
        {{0x81, 0xD9, 0, 0, 0, 0}, ECX},             // sbb ecx,imm32(0)
        {{0x83, 0x16, 0x00}, ESI},                    // adc dword [esi],0
        {{0x83, 0x1F, 0x00}, EDI},                    // sbb dword [edi],0
    };
    for (const auto& c : cases) {
        for (bool carry : {false, true}) {
            for (u32 value : {0x0Fu, 0x10u, 0xFFFFFFFFu}) {
                ASSERT_TRUE(m_memory->write_u32(0x2000, value).is_ok());
                check_parity(c.code.data(), c.code.size(),
                    [=](CpuState& cpu) {
                        cpu.flags.set_cf(carry);
                        cpu.gpr.set32(c.base_reg, c.base_reg == ECX
                            ? value : 0x2000u);
                    },
                    true, false, false, true);
            }
        }
    }
}
