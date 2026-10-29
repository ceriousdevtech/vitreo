// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): MOV, MOVZX and MOVSX
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

// ============================================================================
// MOV parity
// ============================================================================

TEST_F(JitParityTest, MovEaxImm32) {
    check_parity({0xB8, 0x42, 0x00, 0x00, 0x00}); // MOV EAX, 0x42
}

TEST_F(JitParityTest, MovEcxImm32) {
    check_parity({0xB9, 0xEF, 0xBE, 0xAD, 0xDE}); // MOV ECX, 0xDEADBEEF
}

TEST_F(JitParityTest, MovRegReg) {
    check_parity({0x89, 0xD8}, // MOV EAX, EBX
        [](CpuState& cpu) { cpu.gpr.set32(EBX, 0x12345678); });
}

TEST_F(JitParityTest, MovMemToReg) {
    // Need to pre-store value in memory before both runs
    u32 val = 0xCAFEBABE;
    m_memory->write_bytes(0x2000, reinterpret_cast<const u8*>(&val), 4);
    check_parity({0xA1, 0x00, 0x20, 0x00, 0x00}); // MOV EAX, [0x2000]
}

TEST_F(JitParityTest, MovRegToMem) {
    check_parity({0xA3, 0x00, 0x20, 0x00, 0x00}, // MOV [0x2000], EAX
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0xBAADF00D); });
}

TEST_F(JitParityTest, SteamLikeMovzxXorScatterBytes) {
    // Pattern from Steam block 0x006c4816:
    // mov ecx,[esp+2c]; add esp,0xc; mov edx,[esp+14]
    // movzx eax,[ecx+i]; xor al,[esi+edx+i]; mov [esp+esi+48+i],al
    const u32 ecx_base = 0x3000;
    const u32 edx_base = 0x4000;
    const u32 src_a[2] = {0x000000AAu, 0x000000BBu};
    const u32 src_b[2] = {0x00000011u, 0x00000022u};

    ASSERT_TRUE(m_memory->write_u32(0x202c, ecx_base)); // read before ESP += 0x0c
    ASSERT_TRUE(m_memory->write_u32(0x2020, edx_base)); // read after ESP += 0x0c
    ASSERT_TRUE(m_memory->write_u8(ecx_base + 0, static_cast<u8>(src_a[0])));
    ASSERT_TRUE(m_memory->write_u8(ecx_base + 1, static_cast<u8>(src_a[1])));
    ASSERT_TRUE(m_memory->write_u8(edx_base + 0x30 + 0, static_cast<u8>(src_b[0])));
    ASSERT_TRUE(m_memory->write_u8(edx_base + 0x30 + 1, static_cast<u8>(src_b[1])));

    check_parity({
        0x8B, 0x4C, 0x24, 0x2C,       // mov ecx,[esp+2c]
        0x83, 0xC4, 0x0C,             // add esp,0xc
        0x8B, 0x54, 0x24, 0x14,       // mov edx,[esp+14]
        0x0F, 0xB6, 0x01,             // movzx eax,byte ptr [ecx]
        0x32, 0x04, 0x16,             // xor al,byte ptr [esi+edx]
        0x88, 0x44, 0x34, 0x48,       // mov [esp+esi+48],al
        0x0F, 0xB6, 0x44, 0x16, 0x01, // movzx eax,byte ptr [esi+edx+1]
        0x32, 0x41, 0x01,             // xor al,byte ptr [ecx+1]
        0x88, 0x44, 0x34, 0x49        // mov [esp+esi+49],al
    }, [](CpuState& cpu) {
        cpu.gpr.set32(ESP, 0x2000);
        cpu.gpr.set32(ESI, 0x30);
    });
}

TEST_F(JitParityTest, AddRm8Imm8FlagsParity) {
    // Ensure lazy flags for ADD r/m8, imm8 are materialized correctly.
    // This used stale flags before Fix(Add8 materialization).
    ASSERT_TRUE(m_memory->write_u8(0x3000, 0xFF));

    check_parity({
        0xB9, 0x00, 0x30, 0x00, 0x00, // mov ecx,0x3000
        0x83, 0xC8, 0x01,             // or eax,0x1 (force ZF=0 before add)
        0x80, 0x01, 0x01              // add byte ptr [ecx],0x1 (0xFF -> 0x00, ZF=1)
    });
}

// Intel SDM Vol. 2A ADD 00 /r: ADD r/m8, r8 — store into r/m8 (reg or mem).
TEST_F(JitParityTest, AddRm8R8RegParity) {
    // 00 D8 => ADD AL, BL  (r/m8=AL, r8=BL)
    check_parity({0x00, 0xD8}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0x40);
        cpu.gpr.set32(EBX, 0x25);
    });
}

TEST_F(JitParityTest, AddRm8R8MemParity) {
    // 00 01 => ADD byte ptr [ecx], AL
    ASSERT_TRUE(m_memory->write_u8(0x3000, 0xF0));
    check_parity({
        0xB9, 0x00, 0x30, 0x00, 0x00, // mov ecx,0x3000
        0x00, 0x01                    // add [ecx], al
    }, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0x20);
    });
}

TEST_F(JitParityTest, XorRm8R8HighByteParity) {
    // 30 E3 => XOR BL, AH  (r/m8=BL, r8=AH) — exercises high-byte GPR access
    check_parity({0x30, 0xE3}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0xAA00);
        cpu.gpr.set32(EBX, 0x55);
    });
}

TEST_F(JitParityTest, SubRm8R8MemParity) {
    // 28 01 => SUB byte ptr [ecx], AL
    ASSERT_TRUE(m_memory->write_u8(0x3000, 0x50));
    check_parity({
        0xB9, 0x00, 0x30, 0x00, 0x00, // mov ecx,0x3000
        0x28, 0x01                    // sub [ecx], al
    }, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0x10);
    });
}

TEST_F(JitParityTest, MovEaxFsMoffs32) {
    // 64 A1 imm32 => MOV EAX, FS:[moffs32]
    // Matches TLS-style absolute FS references seen in Steam callback code.
    const u32 fs_base = 0x3000;
    const u32 moffs = 0x002c;
    const u32 val = 0xA1B2C3D4;
    m_memory->write_bytes(fs_base + moffs, reinterpret_cast<const u8*>(&val), 4);
    check_parity({0x64, 0xA1, 0x2C, 0x00, 0x00, 0x00},
        [fs_base](CpuState& cpu) {
            cpu.seg.fs_desc.base = fs_base;
        });
}

TEST_F(JitParityTest, MovFsMoffs32Eax) {
    // 64 A3 imm32 => MOV FS:[moffs32], EAX
    const u32 fs_base = 0x4000;
    const u32 moffs = 0x0030;
    check_parity({0x64, 0xA3, 0x30, 0x00, 0x00, 0x00},
        [fs_base](CpuState& cpu) {
            cpu.seg.fs_desc.base = fs_base;
            cpu.gpr.set32(EAX, 0xDEADBEEF);
        });
}

TEST_F(JitParityTest, MovEcxFsDisp32) {
    // 64 8B 0D imm32 => MOV ECX, FS:[disp32]
    // This exact addressing form appears at 0x46abf8 in Steam traces.
    const u32 fs_base = 0x5000;
    const u32 disp = 0x002c;
    const u32 val = 0x11223344;
    m_memory->write_bytes(fs_base + disp, reinterpret_cast<const u8*>(&val), 4);
    check_parity({0x64, 0x8B, 0x0D, 0x2C, 0x00, 0x00, 0x00},
        [fs_base](CpuState& cpu) {
            cpu.seg.fs_desc.base = fs_base;
        });
}

TEST_F(JitParityTest, MovHighByteImmAffectsParentReg) {
    // B4 imm8 => MOV AH, imm8
    check_parity({0xB8, 0x44, 0x33, 0x22, 0x11, 0xB4, 0xAA},
        {}, false);
}

TEST_F(JitParityTest, MovReg8FromHighByte) {
    // 8A DC => MOV BL, AH
    check_parity({0xB8, 0x44, 0x33, 0x22, 0x11, 0x8A, 0xDC},
        {}, false);
}

TEST_F(JitParityTest, MovzxFromHighByteReg) {
    // 0F B6 CC => MOVZX ECX, AH
    check_parity({0xB8, 0x44, 0x33, 0x22, 0x11, 0x0F, 0xB6, 0xCC},
        {}, false);
}

TEST_F(JitParityTest, MovEbpMemLoad) {
    u32 val = 0x11223344;
    m_memory->write_bytes(0x5000, reinterpret_cast<const u8*>(&val), 4);
    // MOV EAX, [EBP-4]  — 8B 45 FC
    check_parity({0x8B, 0x45, 0xFC},
        [](CpuState& cpu) { cpu.gpr.set32(EBP, 0x5004); });
}

TEST_F(JitParityTest, MovEbpMemStore) {
    // MOV [EBP-4], EAX  — 89 45 FC
    check_parity({0x89, 0x45, 0xFC},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBP, 0x5004);
            cpu.gpr.set32(EAX, 0xAABBCCDD);
        });
}

// ============================================================================
// MOVZX / MOVSX parity
// ============================================================================

TEST_F(JitParityTest, MovzxR32R8) {
    check_parity({0x0F, 0xB6, 0xC3}, // MOVZX EAX, BL
        [](CpuState& cpu) { cpu.gpr.set32(EBX, 0xFFFFFF80); },
        false);
}

TEST_F(JitParityTest, MovzxR16R8) {
    // 66 0F B6 C3 => MOVZX AX, BL — preserves EAX[31:16]
    check_parity({0x66, 0x0F, 0xB6, 0xC3},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xABCD0000);
            cpu.gpr.set32(EBX, 0xFFFFFF80);
        },
        false);
}

TEST_F(JitParityTest, MovsxR32R8) {
    check_parity({0x0F, 0xBE, 0xC3}, // MOVSX EAX, BL
        [](CpuState& cpu) { cpu.gpr.set32(EBX, 0x00000080); },
        false);
}

TEST_F(JitParityTest, MovsxR16R8) {
    // 66 0F BE C3 => MOVSX AX, BL — preserves EAX[31:16]
    check_parity({0x66, 0x0F, 0xBE, 0xC3},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xABCD0000);
            cpu.gpr.set32(EBX, 0x00000080);
        },
        false);
}

TEST_F(JitParityTest, Cbw) {
    // 66 98 => CBW: AX = sign-extend(AL); preserves EAX[31:16]
    check_parity({0x66, 0x98},
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0x12340080); },
        false);
}

TEST_F(JitParityTest, Cwd) {
    // 66 99 => CWD: DX:AX = sign-extend(AX); preserves EDX[31:16]
    check_parity({0x66, 0x99},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xABCD8000);
            cpu.gpr.set32(EDX, 0x12345678);
        },
        false);
}

TEST_F(JitParityTest, Cwde) {
    // 98 => CWDE: EAX = sign-extend(AX)
    check_parity({0x98},
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0x00008000); },
        false);
}

TEST_F(JitParityTest, MovzxR32R16) {
    // MOVZX EAX, BX — 0F B7 C3
    check_parity({0x0F, 0xB7, 0xC3},
        [](CpuState& cpu) { cpu.gpr.set32(EBX, 0xFFFF8000); },
        false);
}

TEST_F(JitParityTest, MovsxR32R16) {
    // MOVSX EAX, BX — 0F BF C3
    check_parity({0x0F, 0xBF, 0xC3},
        [](CpuState& cpu) { cpu.gpr.set32(EBX, 0x00008000); },
        false);
}
