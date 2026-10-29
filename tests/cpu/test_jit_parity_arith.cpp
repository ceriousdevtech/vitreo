// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): ADD/SUB/logic/CMP/INC/DEC, NOT/NEG, BTS, BSF/BSR
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

// ============================================================================
// ADD parity (critical for flag behavior)
// ============================================================================

TEST_F(JitParityTest, AddRegReg) {
    check_parity({0x01, 0xD8}, // ADD EAX, EBX
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 100);
            cpu.gpr.set32(EBX, 50);
        });
}

TEST_F(JitParityTest, AddRegImm8) {
    check_parity({0x83, 0xC0, 0x05}, // ADD EAX, 5
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 100); });
}

TEST_F(JitParityTest, AddProducesZero) {
    check_parity({0x01, 0xD8}, // ADD EAX, EBX
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 5);
            cpu.gpr.set32(EBX, static_cast<u32>(-5));
        });
}

TEST_F(JitParityTest, AddCarryOverflow) {
    check_parity({0x01, 0xD8}, // ADD EAX, EBX
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xFFFFFFFF);
            cpu.gpr.set32(EBX, 1);
        });
}

TEST_F(JitParityTest, AddSignedOverflow) {
    check_parity({0x01, 0xD8}, // ADD EAX, EBX
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x7FFFFFFF);
            cpu.gpr.set32(EBX, 1);
        });
}

TEST_F(JitParityTest, AddLargeImm32) {
    // ADD EAX, 0x10000 — 05 00000100
    check_parity({0x05, 0x00, 0x00, 0x01, 0x00},
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0xFFFF0000); });
}

// ============================================================================
// SUB parity
// ============================================================================

TEST_F(JitParityTest, SubRegReg) {
    check_parity({0x29, 0xD8}, // SUB EAX, EBX
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 100);
            cpu.gpr.set32(EBX, 30);
        });
}

TEST_F(JitParityTest, SubBorrow) {
    check_parity({0x29, 0xD8}, // SUB EAX, EBX
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 5);
            cpu.gpr.set32(EBX, 10);
        });
}

TEST_F(JitParityTest, SubSelfZero) {
    check_parity({0x29, 0xC0}, // SUB EAX, EAX
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 42); });
}

// ============================================================================
// AND / OR / XOR parity
// ============================================================================

TEST_F(JitParityTest, AndRegReg) {
    check_parity({0x21, 0xD8}, // AND EAX, EBX
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xFF00FF00);
            cpu.gpr.set32(EBX, 0x0FF00FF0);
        });
}

TEST_F(JitParityTest, OrRegReg) {
    check_parity({0x09, 0xD8}, // OR EAX, EBX
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xFF00FF00);
            cpu.gpr.set32(EBX, 0x00FF00FF);
        });
}

TEST_F(JitParityTest, XorZeroIdiom) {
    check_parity({0x31, 0xC0}, // XOR EAX, EAX
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0xDEADBEEF); });
}

TEST_F(JitParityTest, XorRegReg) {
    check_parity({0x31, 0xD8}, // XOR EAX, EBX
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xAAAAAAAA);
            cpu.gpr.set32(EBX, 0x55555555);
        });
}

TEST_F(JitParityTest, AndImm8) {
    // AND EAX, 0x0F — 83 E0 0F
    check_parity({0x83, 0xE0, 0x0F},
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0xFF); });
}

TEST_F(JitParityTest, OrImm8) {
    // OR EAX, 0x0F — 83 C8 0F
    check_parity({0x83, 0xC8, 0x0F},
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0xF0); });
}

// ============================================================================
// CMP / TEST parity (flag-heavy)
// ============================================================================

TEST_F(JitParityTest, CmpEqual) {
    check_parity({0x39, 0xD8}, // CMP EAX, EBX
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 42);
            cpu.gpr.set32(EBX, 42);
        });
}

TEST_F(JitParityTest, CmpLess) {
    check_parity({0x39, 0xD8}, // CMP EAX, EBX
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 10);
            cpu.gpr.set32(EBX, 20);
        });
}

TEST_F(JitParityTest, CmpGreater) {
    check_parity({0x39, 0xD8}, // CMP EAX, EBX
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 20);
            cpu.gpr.set32(EBX, 10);
        });
}

TEST_F(JitParityTest, CmpSignedOverflow) {
    check_parity({0x39, 0xD8}, // CMP EAX, EBX
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x80000000);
            cpu.gpr.set32(EBX, 1);
        });
}

TEST_F(JitParityTest, CmpImm8) {
    // CMP EAX, 0x42 — 83 F8 42
    check_parity({0x83, 0xF8, 0x42},
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0x42); });
}

// Spotify/CEF OnceCallback guard: cmp byte [ecx+0x3c], 1; je +2
// (libcef+0xe38611). Terminal JE RIP must match interpreter for flag 0/1.
TEST_F(JitParityTest, CmpByteMemImm8JeParity) {
    const std::vector<u8> code{0x80, 0x79, 0x3C, 0x01, 0x74, 0x02};
    for (u8 flag : {u8{0}, u8{1}}) {
        JitCompiler jit(*m_memory);
        ASSERT_TRUE(m_memory->write_u8(0x203C, flag).is_ok());
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
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
        ASSERT_EQ(block.instruction_count, 2u);

        auto setup = [&](CpuState& c) {
            c.init_protected_mode();
            c.gpr.set32(ESP, 0x10000);
            c.rip = 0x1000;
            c.gpr.set32(ECX, 0x2000);
        };
        CpuState icpu;
        setup(icpu);
        {
            Interpreter interp(icpu, *m_memory);
            (void)interp.run(2);
        }
        ASSERT_TRUE(m_memory->write_u8(0x203C, flag).is_ok());
        CpuState jcpu;
        setup(jcpu);
        ASSERT_TRUE(m_memory
                        ->protect_region(0x1000, 0x1000,
                                         MemoryProtection::ReadExecute)
                        .is_ok());
        CompiledBlock* cb = jit.compile(block);
        ASSERT_NE(cb, nullptr);
        (void)test::execute_like_dispatcher(jit, *cb, jcpu);
        ASSERT_TRUE(
            m_memory->protect_region(0x1000, 0x1000, MemoryProtection::ReadWrite)
                .is_ok());
        EXPECT_EQ(static_cast<u32>(icpu.rip), static_cast<u32>(jcpu.rip))
            << "cmp byte [ecx+0x3c],1; je diverged for flag=" << unsigned(flag)
            << " interp=0x" << std::hex << static_cast<u32>(icpu.rip)
            << " jit=0x" << static_cast<u32>(jcpu.rip);
    }
}

TEST_F(JitParityTest, TestZero) {
    check_parity({0x85, 0xC0}, // TEST EAX, EAX
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0); });
}

TEST_F(JitParityTest, TestNonZero) {
    check_parity({0x85, 0xC0}, // TEST EAX, EAX
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0x80000000); });
}

TEST_F(JitParityTest, TestImm32) {
    // TEST EAX, 0xFF — A9 FF000000
    check_parity({0xA9, 0xFF, 0x00, 0x00, 0x00},
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0x100); });
}

// ============================================================================
// INC / DEC parity (partial flag updates — CF preserved!)
// ============================================================================

TEST_F(JitParityTest, IncEax) {
    check_parity({0x40}, // INC EAX (32-bit short form)
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 41); });
}

TEST_F(JitParityTest, DecEax) {
    check_parity({0x48}, // DEC EAX (32-bit short form)
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 1); });
}

TEST_F(JitParityTest, IncOverflow) {
    check_parity({0x40}, // INC EAX
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0x7FFFFFFF); });
}

TEST_F(JitParityTest, IncWrap) {
    check_parity({0x40}, // INC EAX
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0xFFFFFFFF); });
}

TEST_F(JitParityTest, IncDecBytePreserveCarryAndPartialRegister) {
    // INC AL; DEC AH. Exercise byte wrap/overflow and high-byte insertion.
    check_parity({0xFE, 0xC0, 0xFE, 0xCC},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x123480FFu);
            cpu.flags.set_cf(true);
        });
}

TEST_F(JitParityTest, IncDecWordMemoryPreserveCarry) {
    ASSERT_TRUE(m_memory->write_u16(0x2000, 0x7FFF).is_ok());
    ASSERT_TRUE(m_memory->write_u16(0x2002, 0x0000).is_ok());
    // INC word ptr [0x2000]; DEC word ptr [0x2002].
    check_parity({0x66, 0xFF, 0x05, 0x00, 0x20, 0x00, 0x00,
                  0x66, 0xFF, 0x0D, 0x02, 0x20, 0x00, 0x00},
        [](CpuState& cpu) { cpu.flags.set_cf(true); });
}

TEST_F(JitParityTest, IncDecDwordMemoryAddsOne) {
    // INC/DEC r/m32 must add/sub 1 after emit_guest_load32, which clobbers W3.
    ASSERT_TRUE(m_memory->write_u32(0x2000, 0x41).is_ok());
    check_parity({0xFF, 0x00},  // INC dword [EAX]
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x2000);
            cpu.flags.set_cf(true);
        });
    ASSERT_TRUE(m_memory->write_u32(0x2000, 0x41).is_ok());
    check_parity({0xFF, 0x08},  // DEC dword [EAX]
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x2000);
            cpu.flags.set_cf(true);
        });
}

TEST_F(JitParityTest, DecToZero) {
    check_parity({0x48}, // DEC EAX
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 1); });
}

TEST_F(JitParityTest, DecUnderflow) {
    check_parity({0x48}, // DEC EAX
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0x80000000); });
}

// ============================================================================
// NOT / NEG parity
// ============================================================================

TEST_F(JitParityTest, NotReg) {
    check_parity({0xF7, 0xD0}, // NOT EAX
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0x00FF00FF); },
        false); // NOT doesn't affect flags
}

TEST_F(JitParityTest, NegReg) {
    check_parity({0xF7, 0xD8}, // NEG EAX
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 42); });
}

TEST_F(JitParityTest, NegZero) {
    check_parity({0xF7, 0xD8}, // NEG EAX
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0); });
}

TEST_F(JitParityTest, BtsMem16Imm8) {
    // Intel SDM Vol. 2 BTS r/m16, imm8: CF := old bit; then set the bit.
    // 66 0F BA /5 ib — BTS word [EBX], 3
    ASSERT_TRUE(m_memory->write_u32(0x3000, 0x00000001u));
    check_parity({0x66, 0x0F, 0xBA, 0x2B, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x3000);
        });
}

TEST_F(JitParityTest, BtsAxCx) {
    // 66 0F AB C8 — BTS AX, CX. Bit 3 already set so CF=1; upper EAX bits
    // must survive the 16-bit store (emit_store_gpr16, not a 32-bit write).
    check_parity({0x66, 0x0F, 0xAB, 0xC8},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xFFFF0008u);
            cpu.gpr.set32(ECX, 3);
        });
}

// ============================================================================
// BSF / BSR r16 native emit parity
// ============================================================================

TEST_F(JitParityTest, Bsf16FindsBit) {
    // BSF AX, CX — 66 0F BC C1; CF/PF/SF undefined — compare GPRs + ZF via
    // check_flags=false (native path mirrors R32 ZF-only flag write).
    check_parity({0x66, 0x0F, 0xBC, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xDEAD0000u);
            cpu.gpr.set32(ECX, 0x0040u); // bit 6
        },
        false);
}

TEST_F(JitParityTest, Bsf16ZeroPreservesDest) {
    // ZF=1: dest unchanged including upper 16 (0xDEAD stays).
    check_parity({0x66, 0x0F, 0xBC, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xDEADBEEFu);
            cpu.gpr.set32(ECX, 0);
            cpu.flags.set_cf(false);
        });
}

TEST_F(JitParityTest, Bsr16FindsBit) {
    // BSR AX, CX — 66 0F BD C1
    check_parity({0x66, 0x0F, 0xBD, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xCAFE0000u);
            cpu.gpr.set32(ECX, 0x0280u); // bit 9
        },
        false);
}

TEST_F(JitParityTest, Bsr16ZeroPreservesDest) {
    check_parity({0x66, 0x0F, 0xBD, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xCAFEBABEu);
            cpu.gpr.set32(ECX, 0);
            cpu.flags.set_cf(false);
        });
}
