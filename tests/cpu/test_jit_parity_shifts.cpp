// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): shifts and double shifts
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

// ============================================================================
// SHL / SHR / SAR parity
// ============================================================================

TEST_F(JitParityTest, ShlImm) {
    check_parity({0xC1, 0xE0, 0x04}, // SHL EAX, 4
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0x12345678); });
}

TEST_F(JitParityTest, ShrImm) {
    check_parity({0xC1, 0xE8, 0x04}, // SHR EAX, 4
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0x12345678); });
}

TEST_F(JitParityTest, SarImm) {
    check_parity({0xC1, 0xF8, 0x04}, // SAR EAX, 4
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0x80000000); });
}

TEST_F(JitParityTest, ShlByOne) {
    // SHL EAX, 1 — D1 E0
    check_parity({0xD1, 0xE0},
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0x40000000); });
}

TEST_F(JitParityTest, ShrByOne) {
    // SHR EAX, 1 — D1 E8
    check_parity({0xD1, 0xE8},
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 1); });
}

TEST_F(JitParityTest, NativeShrByteByOneFlagsAndPartialRegister) {
    // SHR AL,1 — D0 E8. 0x81 produces CF=1, OF=1 and result 0x40 while
    // preserving EAX[31:8].
    check_parity({0xD0, 0xE8},
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0xA5A5A581u); });
}

TEST_F(JitParityTest, NativeShrByteMemory) {
    ASSERT_TRUE(m_memory->write_u8(0x3003, 0xA5u).is_ok());
    // SHR byte ptr [EBX+3],1.
    check_parity({0xD0, 0x6B, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x3000);
        });
}

// ============================================================================
// SHLD / SHRD r/m32 native emit parity
// ============================================================================

TEST_F(JitParityTest, ShldImmCount0PreservesDestAndFlags) {
    // SHLD EAX, EDX, 0 — no-op; flags and EAX unchanged.
    check_parity({0x0F, 0xA4, 0xD0, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x12345678u);
            cpu.gpr.set32(EDX, 0x9ABCDEF0u);
            cpu.flags.set_cf(true);
            cpu.flags.set_of(true);
            cpu.flags.set_zf(true);
            cpu.flags.set_sf(false);
        });
}

TEST_F(JitParityTest, ShldImmCount1Overflow) {
    // SHLD EAX, EDX, 1 — OF = sign change of DEST.
    check_parity({0x0F, 0xA4, 0xD0, 0x01},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x40000000u);
            cpu.gpr.set32(EDX, 0x80000000u);
        });
}

TEST_F(JitParityTest, ShldImmMultiBit) {
    // SHLD EAX, EDX, 4
    check_parity({0x0F, 0xA4, 0xD0, 0x04},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x12345678u);
            cpu.gpr.set32(EDX, 0x9ABCDEF0u);
            cpu.flags.set_of(true); // must survive (OF undefined / preserved)
        });
}

TEST_F(JitParityTest, ShrdImmCount0PreservesDestAndFlags) {
    check_parity({0x0F, 0xAC, 0xD0, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x12345678u);
            cpu.gpr.set32(EDX, 0x9ABCDEF0u);
            cpu.flags.set_cf(true);
            cpu.flags.set_of(true);
            cpu.flags.set_zf(false);
            cpu.flags.set_sf(true);
        });
}

TEST_F(JitParityTest, ShrdImmCount1Overflow) {
    // SHRD EAX, EDX, 1 — OF from DEST sign vs result sign.
    check_parity({0x0F, 0xAC, 0xD0, 0x01},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x80000001u);
            cpu.gpr.set32(EDX, 0x00000000u);
        });
}

TEST_F(JitParityTest, ShrdImmMultiBit) {
    // SHRD EAX, EDX, 30 — Ed25519-style wide shift.
    check_parity({0x0F, 0xAC, 0xD0, 0x1E},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x12345678u);
            cpu.gpr.set32(EDX, 0x9ABCDEF0u);
        });
}

TEST_F(JitParityTest, ShldByCl) {
    // SHLD EAX, EDX, CL
    check_parity({0x0F, 0xA5, 0xD0},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x11112222u);
            cpu.gpr.set32(EDX, 0x33334444u);
            cpu.gpr.set32(ECX, 5);
        });
}

TEST_F(JitParityTest, ShrdByClCount0Preserves) {
    // SHRD EAX, EDX, CL with CL=0
    check_parity({0x0F, 0xAD, 0xD0},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xDEADBEEFu);
            cpu.gpr.set32(EDX, 0xCAFEBABEu);
            cpu.gpr.set32(ECX, 0);
            cpu.flags.set_cf(true);
            cpu.flags.set_zf(true);
        });
}

TEST_F(JitParityTest, ShrdByClMultiBit) {
    check_parity({0x0F, 0xAD, 0xD0},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x12345678u);
            cpu.gpr.set32(EDX, 0x9ABCDEF0u);
            cpu.gpr.set32(ECX, 12);
        });
}

TEST_F(JitParityTest, ShldMemoryImm) {
    ASSERT_TRUE(m_memory->write_u32(0x3000, 0x11112222u).is_ok());
    // SHLD [EBX], EDX, 8
    check_parity({0x0F, 0xA4, 0x13, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x3000);
            cpu.gpr.set32(EDX, 0xAABBCCDDu);
        });
}

// ---- 8-bit SHL / SHR / SAR ----

TEST_F(JitParityTest, ShlRm8By1_CarryOut) {
    // SHL AL, 1 (D0 /4)
    check_parity({0xD0, 0xE0},
        [](CpuState& c) { c.gpr.set32(EAX, 0x80u); });
}

TEST_F(JitParityTest, ShlRm8Imm8) {
    // SHL AL, 3 (C0 /4 ib)
    check_parity({0xC0, 0xE0, 0x03},
        [](CpuState& c) { c.gpr.set32(EAX, 0x11u); });
}

TEST_F(JitParityTest, ShlRm8Cl) {
    check_parity({0xD2, 0xE0},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x42u);
            c.gpr.set32(ECX, 2);
        });
}

TEST_F(JitParityTest, ShrRm8Imm8) {
    check_parity({0xC0, 0xE8, 0x02},
        [](CpuState& c) { c.gpr.set32(EAX, 0xF0u); });
}

TEST_F(JitParityTest, ShrRm8Cl) {
    check_parity({0xD2, 0xE8},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0xAAu);
            c.gpr.set32(ECX, 3);
        });
}

TEST_F(JitParityTest, SarRm8By1) {
    check_parity({0xD0, 0xF8},
        [](CpuState& c) { c.gpr.set32(EAX, 0x80u); });
}

TEST_F(JitParityTest, SarRm8Imm8) {
    check_parity({0xC0, 0xF8, 0x03},
        [](CpuState& c) { c.gpr.set32(EAX, 0xF0u); });
}

TEST_F(JitParityTest, SarRm8Cl) {
    check_parity({0xD2, 0xF8},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0xC8u);
            c.gpr.set32(ECX, 2);
        });
}

// ---- 16-bit SHL / SHR / SAR / ROL / ROR (COUNT & 1Fh) ----

TEST_F(JitParityTest, ShlRm16By1_CarryOut) {
    // SHL AX, 1 (66 D1 /4)
    check_parity({0x66, 0xD1, 0xE0},
        [](CpuState& c) { c.gpr.set32(EAX, 0x8000u); });
}

TEST_F(JitParityTest, ShlRm16Imm8) {
    // SHL AX, 3 (66 C1 /4 ib)
    check_parity({0x66, 0xC1, 0xE0, 0x03},
        [](CpuState& c) { c.gpr.set32(EAX, 0x1111u); });
}

TEST_F(JitParityTest, ShlRm16Cl) {
    check_parity({0x66, 0xD3, 0xE0},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x4242u);
            c.gpr.set32(ECX, 2);
        });
}

TEST_F(JitParityTest, ShrRm16By1) {
    check_parity({0x66, 0xD1, 0xE8},
        [](CpuState& c) { c.gpr.set32(EAX, 0x8001u); });
}

TEST_F(JitParityTest, ShrRm16Imm8) {
    check_parity({0x66, 0xC1, 0xE8, 0x02},
        [](CpuState& c) { c.gpr.set32(EAX, 0xF0F0u); });
}

TEST_F(JitParityTest, ShrRm16Cl) {
    check_parity({0x66, 0xD3, 0xE8},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0xAAAAu);
            c.gpr.set32(ECX, 3);
        });
}

TEST_F(JitParityTest, SarRm16By1) {
    check_parity({0x66, 0xD1, 0xF8},
        [](CpuState& c) { c.gpr.set32(EAX, 0x8000u); });
}

TEST_F(JitParityTest, SarRm16Imm8) {
    check_parity({0x66, 0xC1, 0xF8, 0x03},
        [](CpuState& c) { c.gpr.set32(EAX, 0xF0F0u); });
}

TEST_F(JitParityTest, SarRm16Cl) {
    check_parity({0x66, 0xD3, 0xF8},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0xC800u);
            c.gpr.set32(ECX, 2);
        });
}

TEST_F(JitParityTest, SarRm16Imm8_WideCountUsesSign) {
    // SAR AX, 16 — CF = sign bit of original (COUNT & 1Fh, count >= width).
    check_parity({0x66, 0xC1, 0xF8, 0x10},
        [](CpuState& c) { c.gpr.set32(EAX, 0x8001u); });
}

TEST_F(JitParityTest, ShlRm16Cl_CountMasked) {
    // CL=0x21 → masked count 1 (COUNT & 1Fh).
    check_parity({0x66, 0xD3, 0xE0},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x4000u);
            c.gpr.set32(ECX, 0x21);
        });
}

// ============================================================================
// SHLD / SHRD r/m16 native emit parity (incl. count ≥ 16)
// ============================================================================

TEST_F(JitParityTest, Shld16ImmMultiBit) {
    // SHLD AX, DX, 4 — 66 0F A4 D0 04
    check_parity({0x66, 0x0F, 0xA4, 0xD0, 0x04},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xAAAA1234u);
            cpu.gpr.set32(EDX, 0xBBBB5678u);
        });
}

TEST_F(JitParityTest, Shld16ImmCount16) {
    // SHLD AX, DX, 16 — result = DX; upper EAX preserved
    check_parity({0x66, 0x0F, 0xA4, 0xD0, 0x10},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xAAAA1235u); // dest LSB=1 → CF
            cpu.gpr.set32(EDX, 0xBBBB5678u);
        });
}

TEST_F(JitParityTest, Shld16ImmCount17) {
    check_parity({0x66, 0x0F, 0xA4, 0xD0, 0x11},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xAAAA1234u);
            cpu.gpr.set32(EDX, 0xBBBB5678u);
        });
}

TEST_F(JitParityTest, Shrd16ImmCount16) {
    // SHRD AX, DX, 16
    check_parity({0x66, 0x0F, 0xAC, 0xD0, 0x10},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xAAAA8001u);
            cpu.gpr.set32(EDX, 0xBBBB5678u);
        });
}

TEST_F(JitParityTest, Shrd16ByCl) {
    // SHRD AX, DX, CL
    check_parity({0x66, 0x0F, 0xAD, 0xD0},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xFFFF1234u);
            cpu.gpr.set32(EDX, 0x00005678u);
            cpu.gpr.set32(ECX, 5);
        });
}

TEST_F(JitParityTest, Shld16ByClCount20) {
    // SHLD AX, DX, CL with CL=20 (≥16 path)
    check_parity({0x66, 0x0F, 0xA5, 0xD0},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x11112222u);
            cpu.gpr.set32(EDX, 0x33334444u);
            cpu.gpr.set32(ECX, 20);
        });
}
