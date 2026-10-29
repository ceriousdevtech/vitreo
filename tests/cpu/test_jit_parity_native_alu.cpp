// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): native 16-bit and 8-bit ALU forms
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

// PE32 native 16-bit ADD/SUB/OR (66h operand-size). Force no delegate so a
// missing stencil fails compile instead of silently matching via interpreter.
TEST_F(JitParityTest, NativeAddRm16R16) {
    // 66 01 D8 => ADD AX, BX
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x66, 0x01, 0xD8}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0x00007FFFu);
        cpu.gpr.set32(EBX, 1);
    });
}

// Intel SDM Vol. 2 XLAT: AL := DS:[EBX + unsigned AL]. Native PE32 stencil.
TEST_F(JitParityTest, NativeXlatAlFromEbxTable) {
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    ASSERT_TRUE(m_memory->write_u8(0x2008, 0xA7).is_ok());
    check_parity({0xD7}, [](CpuState& cpu) {
        cpu.gpr.set32(EBX, 0x2000);
        cpu.gpr.set32(EAX, 0x12345608);
        cpu.flags.rflags = 0x8D7;
    }, false);
}

TEST_F(JitParityTest, NativeAddR16Rm16Mem) {
    // 66 03 01 => ADD AX, word ptr [ECX]
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    ASSERT_TRUE(m_memory->write_u16(0x3000, 0x0002));
    check_parity({
        0xB9, 0x00, 0x30, 0x00, 0x00, // mov ecx,0x3000
        0x66, 0x03, 0x01              // add ax,[ecx]
    }, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0xFFFF0001u);
    });
}

TEST_F(JitParityTest, NativeAddRm16Imm8) {
    // 66 83 C0 01 => ADD AX, imm8
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x66, 0x83, 0xC0, 0x01}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0xABCDEF00u);
    });
}

TEST_F(JitParityTest, NativeAddAxImm16) {
    // 66 05 34 12 => ADD AX, imm16
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x66, 0x05, 0x34, 0x12}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0x11110001u);
    });
}

TEST_F(JitParityTest, NativeAddRm16Imm16Mem) {
    // 66 81 01 00 80 => ADD word ptr [ECX], imm16
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    ASSERT_TRUE(m_memory->write_u16(0x3000, 0x7FFF));
    check_parity({
        0xB9, 0x00, 0x30, 0x00, 0x00, // mov ecx,0x3000
        0x66, 0x81, 0x01, 0x00, 0x80  // add word ptr [ecx],0x8000
    });
}

TEST_F(JitParityTest, NativeSubRm16R16) {
    // 66 29 D8 => SUB AX, BX — borrow sets CF
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x66, 0x29, 0xD8}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0xAAAABBBBu);
        cpu.gpr.set32(EBX, 0x0000CCCCu);
    });
}

TEST_F(JitParityTest, NativeSubR16Rm16Mem) {
    // 66 2B 01 => SUB AX, word ptr [ECX]
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    ASSERT_TRUE(m_memory->write_u16(0x3000, 1));
    check_parity({
        0xB9, 0x00, 0x30, 0x00, 0x00,
        0x66, 0x2B, 0x01
    }, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0x12340000u);
    });
}

TEST_F(JitParityTest, NativeSubRm16Imm8) {
    // 66 83 E8 FF => SUB AX, -1
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x66, 0x83, 0xE8, 0xFF}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0xFFFF0000u);
    });
}

TEST_F(JitParityTest, NativeSubAxImm16) {
    // 66 2D 01 00 => SUB AX, 1
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x66, 0x2D, 0x01, 0x00}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0xDEAD0000u);
    });
}

TEST_F(JitParityTest, NativeSubRm16Imm16Mem) {
    // 66 81 29 01 00 => SUB word ptr [ECX], imm16
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    ASSERT_TRUE(m_memory->write_u16(0x3000, 0x0001));
    check_parity({
        0xB9, 0x00, 0x30, 0x00, 0x00,
        0x66, 0x81, 0x29, 0x01, 0x00
    });
}

TEST_F(JitParityTest, NativeOrRm16R16) {
    // 66 09 D8 => OR AX, BX — clears CF/OF
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x66, 0x09, 0xD8}, [](CpuState& cpu) {
        cpu.flags.set_cf(true);
        cpu.gpr.set32(EAX, 0xFFFF00F0u);
        cpu.gpr.set32(EBX, 0x0000000Fu);
    });
}

TEST_F(JitParityTest, NativeOrR16Rm16Mem) {
    // 66 0B 01 => OR AX, word ptr [ECX]
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    ASSERT_TRUE(m_memory->write_u16(0x3000, 0x0F00));
    check_parity({
        0xB9, 0x00, 0x30, 0x00, 0x00,
        0x66, 0x0B, 0x01
    }, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0xABCD00F0u);
    });
}

TEST_F(JitParityTest, NativeOrRm16Imm8) {
    // 66 83 C8 80 => OR AX, imm8 (sign-extended)
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x66, 0x83, 0xC8, 0x80}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0x11110001u);
    });
}

TEST_F(JitParityTest, NativeOrAxImm16) {
    // 66 0D FF 00 => OR AX, imm16
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x66, 0x0D, 0xFF, 0x00}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0x22220000u);
    });
}

TEST_F(JitParityTest, NativeOrRm16Imm16Mem) {
    // 66 81 09 00 FF => OR word ptr [ECX], imm16
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    ASSERT_TRUE(m_memory->write_u16(0x3000, 0x00FF));
    check_parity({
        0xB9, 0x00, 0x30, 0x00, 0x00,
        0x66, 0x81, 0x09, 0x00, 0xFF
    });
}

TEST_F(JitParityTest, NativeAdcRm16R16) {
    // 66 11 D8 => ADC AX, BX with CF=1
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x66, 0x11, 0xD8}, [](CpuState& cpu) {
        cpu.flags.set_cf(true);
        cpu.gpr.set32(EAX, 0xAAAAFFFFu);
        cpu.gpr.set32(EBX, 0x00000001u);
    });
}

TEST_F(JitParityTest, NativeAdcR16Rm16Mem) {
    // 66 13 01 => ADC AX, word ptr [ECX]
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    ASSERT_TRUE(m_memory->write_u16(0x3000, 0x0002));
    check_parity({
        0xB9, 0x00, 0x30, 0x00, 0x00,
        0x66, 0x13, 0x01
    }, [](CpuState& cpu) {
        cpu.flags.set_cf(true);
        cpu.gpr.set32(EAX, 0x1234FFFDu);
    });
}

TEST_F(JitParityTest, NativeAdcRm16Imm8) {
    // 66 83 D0 FF => ADC AX, -1 (sign-extended imm8) with CF=1
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x66, 0x83, 0xD0, 0xFF}, [](CpuState& cpu) {
        cpu.flags.set_cf(true);
        cpu.gpr.set32(EAX, 0x11110000u);
    });
}

TEST_F(JitParityTest, NativeAdcAxImm16) {
    // 66 15 01 00 => ADC AX, 1 with CF=1
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x66, 0x15, 0x01, 0x00}, [](CpuState& cpu) {
        cpu.flags.set_cf(true);
        cpu.gpr.set32(EAX, 0xDEADFFFEu);
    });
}

TEST_F(JitParityTest, NativeAdcRm16Imm16Mem) {
    // 66 81 11 00 80 => ADC word ptr [ECX], imm16 with CF=1
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    ASSERT_TRUE(m_memory->write_u16(0x3000, 0x7FFF));
    check_parity({
        0xB9, 0x00, 0x30, 0x00, 0x00,
        0x66, 0x81, 0x11, 0x00, 0x80
    }, [](CpuState& cpu) {
        cpu.flags.set_cf(true);
    });
}

TEST_F(JitParityTest, NativeSbbRm16R16) {
    // 66 19 D8 => SBB AX, BX with CF=1
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x66, 0x19, 0xD8}, [](CpuState& cpu) {
        cpu.flags.set_cf(true);
        cpu.gpr.set32(EAX, 0xAAAA0000u);
        cpu.gpr.set32(EBX, 0x00000001u);
    });
}

TEST_F(JitParityTest, NativeSbbR16Rm16Mem) {
    // 66 1B 01 => SBB AX, word ptr [ECX]
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    ASSERT_TRUE(m_memory->write_u16(0x3000, 0x0001));
    check_parity({
        0xB9, 0x00, 0x30, 0x00, 0x00,
        0x66, 0x1B, 0x01
    }, [](CpuState& cpu) {
        cpu.flags.set_cf(true);
        cpu.gpr.set32(EAX, 0x12340001u);
    });
}

TEST_F(JitParityTest, NativeSbbRm16Imm8) {
    // 66 83 D8 01 => SBB AX, 1 with CF=1
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x66, 0x83, 0xD8, 0x01}, [](CpuState& cpu) {
        cpu.flags.set_cf(true);
        cpu.gpr.set32(EAX, 0xFFFF0002u);
    });
}

TEST_F(JitParityTest, NativeSbbAxImm16) {
    // 66 1D 01 00 => SBB AX, 1 with CF=1
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x66, 0x1D, 0x01, 0x00}, [](CpuState& cpu) {
        cpu.flags.set_cf(true);
        cpu.gpr.set32(EAX, 0xDEAD0001u);
    });
}

TEST_F(JitParityTest, NativeSbbRm16Imm16Mem) {
    // 66 81 19 01 00 => SBB word ptr [ECX], imm16 with CF=1
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    ASSERT_TRUE(m_memory->write_u16(0x3000, 0x0002));
    check_parity({
        0xB9, 0x00, 0x30, 0x00, 0x00,
        0x66, 0x81, 0x19, 0x01, 0x00
    }, [](CpuState& cpu) {
        cpu.flags.set_cf(true);
    });
}

TEST_F(JitParityTest, NativeAndR8Rm8Mem) {
    // 22 01 => AND AL, byte ptr [ECX]
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    ASSERT_TRUE(m_memory->write_u8(0x3000, 0x0F));
    check_parity({
        0xB9, 0x00, 0x30, 0x00, 0x00,
        0x22, 0x01
    }, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0xABCDEF55u);
    });
}

TEST_F(JitParityTest, NativeXorRm8Imm8) {
    // 80 F0 AA => XOR AL, imm8
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x80, 0xF0, 0xAA}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0x11223355u);
    });
}

TEST_F(JitParityTest, NativeAdcRm8Imm8) {
    // 80 D0 01 => ADC AL, imm8 with CF=1
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x80, 0xD0, 0x01}, [](CpuState& cpu) {
        cpu.flags.set_cf(true);
        cpu.gpr.set32(EAX, 0xABCDEFFEu);
    });
}

TEST_F(JitParityTest, NativeAdcAlImm8) {
    // 14 FF => ADC AL, imm8 with CF=1 (wrap)
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x14, 0xFF}, [](CpuState& cpu) {
        cpu.flags.set_cf(true);
        cpu.gpr.set32(EAX, 0x12345601u);
    });
}
