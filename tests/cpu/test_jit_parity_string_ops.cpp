// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): string operations
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

TEST_F(JitParityTest, RepMovsbShortCount) {
    ASSERT_TRUE(m_memory->write_bytes(0x2000,
        (const u8*)"\x11\x22\x33\x44\x55", 5).is_ok());
    check_parity({0xF3, 0xA4},
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x2000);
            cpu.gpr.set32(EDI, 0x3000);
            cpu.gpr.set32(ECX, 5);
        },
        false);
}


TEST_F(JitParityTest, StosdDfClear) {
    // MOV EAX, 0x12345678
    // MOV EDI, 0x2004
    // STOSD                    ; [0x2004] = EAX, EDI += 4
    // MOV ECX, [EDI-4]
    check_parity({
        0xB8, 0x78, 0x56, 0x34, 0x12,
        0xBF, 0x04, 0x20, 0x00, 0x00,
        0xAB,
        0x8B, 0x4F, 0xFC,
    });
}

TEST_F(JitParityTest, StosdDfSet) {
    // MOV EAX, 0x89ABCDEF
    // MOV EDI, 0x2008
    // STOSD                    ; [0x2008] = EAX, EDI -= 4
    // MOV ECX, [EDI+4]
    check_parity({
        0xB8, 0xEF, 0xCD, 0xAB, 0x89,
        0xBF, 0x08, 0x20, 0x00, 0x00,
        0xAB,
        0x8B, 0x4F, 0x04,
    },
        [](CpuState& cpu) {
            cpu.flags.set_df(true);
        },
    false);
}

TEST_F(JitParityTest, LodsbDfClear) {
    // LODSB — AC: AL = [ESI], ESI += 1 (DF clear)
    ASSERT_TRUE(m_memory->write_u8(0x2000, 0x5A).is_ok());
    check_parity({0xAC},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x11223344u);
            cpu.gpr.set32(ESI, 0x2000);
        },
        false);
}

TEST_F(JitParityTest, LodswDfClear) {
    // LODSW — 66 AD: AX = [ESI], ESI += 2
    ASSERT_TRUE(m_memory->write_u16(0x2000, 0xBEEF).is_ok());
    check_parity({0x66, 0xAD},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x11223344u);
            cpu.gpr.set32(ESI, 0x2000);
        },
        false);
}

TEST_F(JitParityTest, LodsdDfSet) {
    // LODSD — AD: EAX = [ESI], ESI -= 4 (DF set)
    ASSERT_TRUE(m_memory->write_u32(0x2008, 0xA1B2C3D4u).is_ok());
    check_parity({0xAD},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0);
            cpu.gpr.set32(ESI, 0x2008);
            cpu.flags.set_df(true);
        },
        false);
}

TEST_F(JitParityTest, RepLodsb) {
    // REP LODSB — F3 AC: ECX loads from [ESI] into AL
    ASSERT_TRUE(m_memory->write_u8(0x2000, 0x11).is_ok());
    ASSERT_TRUE(m_memory->write_u8(0x2001, 0x22).is_ok());
    ASSERT_TRUE(m_memory->write_u8(0x2002, 0x33).is_ok());
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0xF3, 0xAC},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xAABBCC00u);
            cpu.gpr.set32(ESI, 0x2000);
            cpu.gpr.set32(ECX, 3);
        },
        false);
}

TEST_F(JitParityTest, RepLodsdDfSet) {
    // REP LODSD — F3 AD with DF set
    ASSERT_TRUE(m_memory->write_u32(0x2010, 0x11111111u).is_ok());
    ASSERT_TRUE(m_memory->write_u32(0x200C, 0x22222222u).is_ok());
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0xF3, 0xAD},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0);
            cpu.gpr.set32(ESI, 0x2010);
            cpu.gpr.set32(ECX, 2);
            cpu.flags.set_df(true);
        },
        false);
}

// ============================================================================
// PE32 string ops: MOVSW / CMPSW/D / SCASW/D (REP* ZF early-exit)
// ============================================================================

TEST_F(JitParityTest, MovswNonRepDfClear) {
    // MOVSW — 66 A5
    ASSERT_TRUE(m_memory->write_u16(0x3000, 0xBEEFu).is_ok());
    check_parity({0x66, 0xA5},
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            cpu.gpr.set32(EDI, 0x3100);
            cpu.flags.set_df(false);
        });
}

TEST_F(JitParityTest, MovswNonRepDfSet) {
    ASSERT_TRUE(m_memory->write_u16(0x3002, 0xCAFE).is_ok());
    check_parity({0x66, 0xA5},
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3002);
            cpu.gpr.set32(EDI, 0x3102);
            cpu.flags.set_df(true);
        });
}

TEST_F(JitParityTest, CmpswNonRep) {
    // CMPSW — 66 A7
    ASSERT_TRUE(m_memory->write_u16(0x3000, 0x1111).is_ok());
    ASSERT_TRUE(m_memory->write_u16(0x3100, 0x2222).is_ok());
    check_parity({0x66, 0xA7},
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            cpu.gpr.set32(EDI, 0x3100);
        });
}

TEST_F(JitParityTest, CmpsdNonRepEqual) {
    // CMPSD — A7
    ASSERT_TRUE(m_memory->write_u32(0x3000, 0xAABBCCDDu).is_ok());
    ASSERT_TRUE(m_memory->write_u32(0x3100, 0xAABBCCDDu).is_ok());
    check_parity({0xA7},
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            cpu.gpr.set32(EDI, 0x3100);
        });
}

TEST_F(JitParityTest, RepeCmpswStopsAtMismatch) {
    // REPE CMPSW — F3 66 A7
    const u16 src[] = {0x1111, 0x2222, 0x3333, 0x4444};
    const u16 dst[] = {0x1111, 0x2222, 0x9999, 0x4444};
    ASSERT_TRUE(m_memory->write_bytes(0x3000, reinterpret_cast<const u8*>(src),
                                      sizeof(src)).is_ok());
    ASSERT_TRUE(m_memory->write_bytes(0x3100, reinterpret_cast<const u8*>(dst),
                                      sizeof(dst)).is_ok());
    check_parity({0xF3, 0x66, 0xA7},
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            cpu.gpr.set32(EDI, 0x3100);
            cpu.gpr.set32(ECX, 4);
        });
}

TEST_F(JitParityTest, RepeCmpsdAllEqual) {
    // REPE CMPSD — F3 A7
    const u32 src[] = {1, 2, 3};
    const u32 dst[] = {1, 2, 3};
    ASSERT_TRUE(m_memory->write_bytes(0x3000, reinterpret_cast<const u8*>(src),
                                      sizeof(src)).is_ok());
    ASSERT_TRUE(m_memory->write_bytes(0x3100, reinterpret_cast<const u8*>(dst),
                                      sizeof(dst)).is_ok());
    check_parity({0xF3, 0xA7},
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            cpu.gpr.set32(EDI, 0x3100);
            cpu.gpr.set32(ECX, 3);
        });
}

TEST_F(JitParityTest, RepneCmpswStopsAtMatch) {
    // REPNE CMPSW — F2 66 A7
    const u16 src[] = {0xAAAA, 0xBBBB, 0xCCCC};
    const u16 dst[] = {0x1111, 0xBBBB, 0x2222};
    ASSERT_TRUE(m_memory->write_bytes(0x3000, reinterpret_cast<const u8*>(src),
                                      sizeof(src)).is_ok());
    ASSERT_TRUE(m_memory->write_bytes(0x3100, reinterpret_cast<const u8*>(dst),
                                      sizeof(dst)).is_ok());
    check_parity({0xF2, 0x66, 0xA7},
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            cpu.gpr.set32(EDI, 0x3100);
            cpu.gpr.set32(ECX, 3);
        });
}

TEST_F(JitParityTest, RepneCmpsdNoMatch) {
    // REPNE CMPSD — F2 A7
    const u32 src[] = {0x10, 0x20};
    const u32 dst[] = {0x11, 0x21};
    ASSERT_TRUE(m_memory->write_bytes(0x3000, reinterpret_cast<const u8*>(src),
                                      sizeof(src)).is_ok());
    ASSERT_TRUE(m_memory->write_bytes(0x3100, reinterpret_cast<const u8*>(dst),
                                      sizeof(dst)).is_ok());
    check_parity({0xF2, 0xA7},
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            cpu.gpr.set32(EDI, 0x3100);
            cpu.gpr.set32(ECX, 2);
        });
}

TEST_F(JitParityTest, RepneScaswFindsWord) {
    // REPNE SCASW — F2 66 AF
    const u16 buf[] = {0x1111, 0x2222, 0x3333, 0x4444};
    ASSERT_TRUE(m_memory->write_bytes(0x3000, reinterpret_cast<const u8*>(buf),
                                      sizeof(buf)).is_ok());
    check_parity({0xF2, 0x66, 0xAF},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x3333);
            cpu.gpr.set32(EDI, 0x3000);
            cpu.gpr.set32(ECX, 4);
        });
}

TEST_F(JitParityTest, RepeScasdStopsAtMismatch) {
    // REPE SCASD — F3 AF
    const u32 buf[] = {0x55, 0x55, 0x99, 0x55};
    ASSERT_TRUE(m_memory->write_bytes(0x3000, reinterpret_cast<const u8*>(buf),
                                      sizeof(buf)).is_ok());
    check_parity({0xF3, 0xAF},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x55);
            cpu.gpr.set32(EDI, 0x3000);
            cpu.gpr.set32(ECX, 4);
        });
}

TEST_F(JitParityTest, RepneScasdDfSet) {
    // REPNE SCASD backward — F2 AF
    const u32 buf[] = {0x1, 0x2, 0xDEADBEEF, 0x4};
    ASSERT_TRUE(m_memory->write_bytes(0x3000, reinterpret_cast<const u8*>(buf),
                                      sizeof(buf)).is_ok());
    check_parity({0xF2, 0xAF},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xDEADBEEF);
            cpu.gpr.set32(EDI, 0x3008); // points at 0xDEADBEEF
            cpu.gpr.set32(ECX, 3);
            cpu.flags.set_df(true);
        });
}

TEST_F(JitParityTest, RepeScasbStopsAtMismatch) {
    // REPE SCASB — F3 AE
    const u8 buf[] = {0x41, 0x41, 0x42, 0x41};
    ASSERT_TRUE(m_memory->write_bytes(0x3000, buf, sizeof(buf)).is_ok());
    check_parity({0xF3, 0xAE},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x41);
            cpu.gpr.set32(EDI, 0x3000);
            cpu.gpr.set32(ECX, 4);
        });
}

TEST_F(JitParityTest, RepneCmpsbStopsAtMatch) {
    // REPNE CMPSB — F2 A6
    const u8 src[] = {0x10, 0x20, 0x30};
    const u8 dst[] = {0x11, 0x20, 0x31};
    ASSERT_TRUE(m_memory->write_bytes(0x3000, src, sizeof(src)).is_ok());
    ASSERT_TRUE(m_memory->write_bytes(0x3100, dst, sizeof(dst)).is_ok());
    check_parity({0xF2, 0xA6},
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            cpu.gpr.set32(EDI, 0x3100);
            cpu.gpr.set32(ECX, 3);
        });
}
