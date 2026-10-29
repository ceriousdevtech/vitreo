// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): rotates
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

// ---- ROL r/m32, 1 (D1 /0) — the implicit-count rotate that dominates one of
// Steam's decompression checksum loops. OF is defined for a 1-bit rotate
// (OF = MSB(result) XOR CF), so flags must match the interpreter exactly. ----

TEST_F(JitParityTest, RolRm32By1_NoCarryOut) {
    // ROL EAX, 1 with a value whose top bit is 0: CF=0, OF=0.
    check_parity({0xD1, 0xC0},
        [](CpuState& c) { c.gpr.set32(EAX, 0x00000001u); });
}

TEST_F(JitParityTest, RolRm32By1_CarryOutSetsOf) {
    // Top bit set → CF=1; result MSB becomes 0 → OF = 1 XOR 0 = 1.
    check_parity({0xD1, 0xC0},
        [](CpuState& c) { c.gpr.set32(EAX, 0x80000000u); });
}

TEST_F(JitParityTest, RolRm32By1_BothTopBitsSet) {
    // 0xC0000000: CF=1 (old bit31), new MSB=1 → OF = 1 XOR 1 = 0.
    check_parity({0xD1, 0xC0},
        [](CpuState& c) { c.gpr.set32(EAX, 0xC0000000u); });
}

TEST_F(JitParityTest, RolRm32By1_Memory) {
    // ROL dword [EAX], 1 — D1 00. Exercises the memory operand path.
    check_parity({0xD1, 0x00},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x2000u);
        });
}

// ---- RCL / RCR r/m32 — rotate through CF (Intel SDM Vol. 2). ----

TEST_F(JitParityTest, RclRm32By1_InjectsCarry) {
    // RCL EAX, 1 (D1 /2): CF=1 into bit0; MSB out to CF.
    check_parity({0xD1, 0xD0},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x40000000u);
            c.flags.set_cf(true);
        });
}

TEST_F(JitParityTest, RclRm32By1_NoIncomingCarry) {
    check_parity({0xD1, 0xD0},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x80000000u);
            c.flags.set_cf(false);
        });
}

TEST_F(JitParityTest, RclRm32Imm8) {
    // RCL EAX, 3 (C1 /2 ib)
    check_parity({0xC1, 0xD0, 0x03},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x11111111u);
            c.flags.set_cf(true);
        });
}

TEST_F(JitParityTest, RclRm32Cl) {
    // RCL EAX, CL (D3 /2)
    check_parity({0xD3, 0xD0},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0xA5A5A5A5u);
            c.gpr.set32(ECX, 5);
            c.flags.set_cf(true);
        });
}

TEST_F(JitParityTest, RclRm32Cl_CountZeroPreserves) {
    check_parity({0xD3, 0xD0},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x12345678u);
            c.gpr.set32(ECX, 0);
            c.flags.set_cf(true);
            c.flags.set_zf(true);
        });
}

TEST_F(JitParityTest, RcrRm32By1_InjectsCarry) {
    // RCR EAX, 1 (D1 /3)
    check_parity({0xD1, 0xD8},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x00000002u);
            c.flags.set_cf(true);
        });
}

TEST_F(JitParityTest, RcrRm32By1_LowBitToCarry) {
    check_parity({0xD1, 0xD8},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x00000001u);
            c.flags.set_cf(false);
        });
}

TEST_F(JitParityTest, RcrRm32Imm8) {
    check_parity({0xC1, 0xD8, 0x04},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0xF0F0F0F0u);
            c.flags.set_cf(true);
        });
}

TEST_F(JitParityTest, RcrRm32Cl) {
    check_parity({0xD3, 0xD8},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x87654321u);
            c.gpr.set32(ECX, 7);
            c.flags.set_cf(false);
        });
}

// ---- RCL / RCR r/m8 — rotate through CF (9-bit; count%9). ----

TEST_F(JitParityTest, RclRm8By1_InjectsCarry) {
    // RCL AL, 1 (D0 /2)
    check_parity({0xD0, 0xD0},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x40u);
            c.flags.set_cf(true);
        });
}

TEST_F(JitParityTest, RclRm8Imm8) {
    // RCL AL, 3 (C0 /2 ib)
    check_parity({0xC0, 0xD0, 0x03},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x11u);
            c.flags.set_cf(true);
        });
}

TEST_F(JitParityTest, RclRm8Imm8_ModuloWidth) {
    // RCL AL, 9 → tempCOUNT=0; dest/flags unchanged.
    check_parity({0xC0, 0xD0, 0x09},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0xA5u);
            c.flags.set_cf(true);
            c.flags.set_zf(true);
        });
}

TEST_F(JitParityTest, RclRm8Cl) {
    // RCL AL, CL (D2 /2)
    check_parity({0xD2, 0xD0},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0xA5u);
            c.gpr.set32(ECX, 5);
            c.flags.set_cf(true);
        });
}

TEST_F(JitParityTest, RcrRm8By1_InjectsCarry) {
    // RCR AL, 1 (D0 /3)
    check_parity({0xD0, 0xD8},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x02u);
            c.flags.set_cf(true);
        });
}

TEST_F(JitParityTest, RcrRm8Imm8) {
    check_parity({0xC0, 0xD8, 0x04},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0xF0u);
            c.flags.set_cf(true);
        });
}

TEST_F(JitParityTest, RcrRm8Cl) {
    check_parity({0xD2, 0xD8},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x87u);
            c.gpr.set32(ECX, 7);
            c.flags.set_cf(false);
        });
}

// ---- RCL / RCR r/m16 — rotate through CF (17-bit; count%17). ----

TEST_F(JitParityTest, RclRm16By1_InjectsCarry) {
    // RCL AX, 1 (66 D1 /2)
    check_parity({0x66, 0xD1, 0xD0},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x4000u);
            c.flags.set_cf(true);
        });
}

TEST_F(JitParityTest, RclRm16Imm8) {
    // RCL AX, 3 (66 C1 /2 ib)
    check_parity({0x66, 0xC1, 0xD0, 0x03},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x1111u);
            c.flags.set_cf(true);
        });
}

TEST_F(JitParityTest, RclRm16Cl) {
    check_parity({0x66, 0xD3, 0xD0},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0xA5A5u);
            c.gpr.set32(ECX, 5);
            c.flags.set_cf(true);
        });
}

TEST_F(JitParityTest, RcrRm16By1_InjectsCarry) {
    check_parity({0x66, 0xD1, 0xD8},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x0002u);
            c.flags.set_cf(true);
        });
}

TEST_F(JitParityTest, RcrRm16Imm8) {
    check_parity({0x66, 0xC1, 0xD8, 0x04},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0xF0F0u);
            c.flags.set_cf(true);
        });
}

TEST_F(JitParityTest, RcrRm16Cl) {
    check_parity({0x66, 0xD3, 0xD8},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x8765u);
            c.gpr.set32(ECX, 7);
            c.flags.set_cf(false);
        });
}

TEST_F(JitParityTest, RolRm16By1_CarryOut) {
    // ROL AX, 1 (66 D1 /0)
    check_parity({0x66, 0xD1, 0xC0},
        [](CpuState& c) { c.gpr.set32(EAX, 0x8000u); });
}

TEST_F(JitParityTest, RolRm16Imm8) {
    check_parity({0x66, 0xC1, 0xC0, 0x03},
        [](CpuState& c) { c.gpr.set32(EAX, 0x1234u); });
}

TEST_F(JitParityTest, RolRm16Cl) {
    check_parity({0x66, 0xD3, 0xC0},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0xA5A5u);
            c.gpr.set32(ECX, 5);
        });
}

TEST_F(JitParityTest, RorRm16By1_CarryOut) {
    // ROR AX, 1 (66 D1 /1)
    check_parity({0x66, 0xD1, 0xC8},
        [](CpuState& c) { c.gpr.set32(EAX, 0x0001u); });
}

TEST_F(JitParityTest, RorRm16Imm8) {
    check_parity({0x66, 0xC1, 0xC8, 0x04},
        [](CpuState& c) { c.gpr.set32(EAX, 0xF0F0u); });
}

TEST_F(JitParityTest, RorRm16Cl) {
    check_parity({0x66, 0xD3, 0xC8},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x8765u);
            c.gpr.set32(ECX, 7);
        });
}

TEST_F(JitParityTest, RolRm8By1_CarryOut) {
    // ROL AL, 1 (D0 /0)
    check_parity({0xD0, 0xC0},
        [](CpuState& c) { c.gpr.set32(EAX, 0x80u); });
}

TEST_F(JitParityTest, RolRm8Imm8) {
    check_parity({0xC0, 0xC0, 0x03},
        [](CpuState& c) { c.gpr.set32(EAX, 0xA5u); });
}

TEST_F(JitParityTest, RolRm8Cl) {
    check_parity({0xD2, 0xC0},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x5Au);
            c.gpr.set32(ECX, 4);
        });
}

TEST_F(JitParityTest, RorRm8By1_CarryOut) {
    // ROR AL, 1 (D0 /1)
    check_parity({0xD0, 0xC8},
        [](CpuState& c) { c.gpr.set32(EAX, 0x01u); });
}

TEST_F(JitParityTest, RorRm8Imm8) {
    check_parity({0xC0, 0xC8, 0x04},
        [](CpuState& c) { c.gpr.set32(EAX, 0xF0u); });
}

TEST_F(JitParityTest, RorRm8Cl) {
    check_parity({0xD2, 0xC8},
        [](CpuState& c) {
            c.gpr.set32(EAX, 0x87u);
            c.gpr.set32(ECX, 3);
        });
}
