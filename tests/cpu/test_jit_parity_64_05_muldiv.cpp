// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): NOT/NEG, IMUL,
// MUL/DIV and multi-instruction sequences.

#include "jit_parity_64_fixture.hpp"

// ============================================================================
// 64-bit NOT / NEG
// ============================================================================

TEST_F(JitParity64Test, NotR64) {
    // NOT RAX — 48 F7 D0
    check_parity({0x48, 0xF7, 0xD0},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x00FF00FF00FF00FFull); },
        false);
}

TEST_F(JitParity64Test, NegR64) {
    // NEG RAX — 48 F7 D8
    check_parity({0x48, 0xF7, 0xD8},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 42); });
}

// ============================================================================
// 64-bit IMUL
// ============================================================================

TEST_F(JitParity64Test, ImulR64R64) {
    // IMUL RAX, RBX — 48 0F AF C3
    check_parity({0x48, 0x0F, 0xAF, 0xC3},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 7);
            cpu.gpr.set64(RBX, 6);
        });
}

TEST_F(JitParity64Test, ImulR64Rm64Memory) {
    // IMUL RAX, qword ptr [RBX] — 48 0F AF 03
    const u8 value[] = {6, 0, 0, 0, 0, 0, 0, 0};
    ASSERT_TRUE(m_memory->write_bytes(0x2000, value, sizeof(value)));
    check_parity({0x48, 0x0F, 0xAF, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 7);
            cpu.gpr.set64(RBX, 0x2000);
        });
}

// ============================================================================
// 64-bit MUL (one-operand) — CF/OF feed ADC in montgomery loops
// ============================================================================

TEST_F(JitParity64Test, MulR64ProducesHighHalfAndCarry) {
    // MUL RBX — 48 F7 E3
    check_parity({0x48, 0xF7, 0xE3},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x8000000000000001ull);
            cpu.gpr.set64(RBX, 0x0000000000000003ull);
        });
}

TEST_F(JitParity64Test, MulR64ThenAdcUsesMulCarry) {
    // ADD sets CF, then MUL must replace CF for the following ADC.
    // ADD RCX, RDX; MUL RBX; ADC RSI, RDI
    u8 code[] = {
        0x48, 0x01, 0xD1,  // ADD RCX, RDX  (may set CF)
        0x48, 0xF7, 0xE3,  // MUL RBX
        0x48, 0x11, 0xFE,  // ADC RSI, RDI
    };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            cpu.gpr.set64(RCX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RDX, 1);  // ADD overflows → CF=1 before MUL
            cpu.gpr.set64(RAX, 2);
            cpu.gpr.set64(RBX, 3);  // product 6, high=0 → MUL CF=0
            cpu.gpr.set64(RSI, 10);
            cpu.gpr.set64(RDI, 1);  // ADC must add 0, not the pre-MUL CF
        });
}

TEST_F(JitParity64Test, DivRm64Register) {
    // DIV RBX — 48 F7 F3. RDX:RAX = 0x1_0000000000000000 / 3
    check_native_parity({0x48, 0xF7, 0xF3},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0);
            cpu.gpr.set64(RDX, 1);
            cpu.gpr.set64(RBX, 3);
        },
        false);
}

TEST_F(JitParity64Test, DivRm64Memory) {
    // DIV qword ptr [RCX] — 48 F7 31
    ASSERT_TRUE(m_memory->write_u64(0x2000, 10).is_ok());
    check_native_parity({0x48, 0xF7, 0x31},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RDX, 0);
            cpu.gpr.set64(RCX, 0x2000);
        },
        false);
}

TEST_F(JitParity64Test, IdivRm64Register) {
    // IDIV RCX — 48 F7 F9. (-1000) / 7 → quot -142 rem -6
    check_native_parity({0x48, 0xF7, 0xF9},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, static_cast<u64>(static_cast<i64>(-1000)));
            cpu.gpr.set64(RDX, ~0ull);  // sign-extend high
            cpu.gpr.set64(RCX, 7);
        },
        false);
}

TEST_F(JitParity64Test, NativePcmpeqbMemoryUsesFullEa) {
    constexpr GuestAddr data_address = 0x140014000ull;
    map_tracked_region(data_address, 0x1000);
    // Bytes: equal, differ, equal, …
    const u8 mem_bytes[16] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
    };
    ASSERT_TRUE(m_memory->write_bytes(data_address, mem_bytes, 16));
    // PCMPEQB XMM8, [RAX] — 66 44 0F 74 00
    check_native_parity({0x66, 0x44, 0x0F, 0x74, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            const u8 dst[16] = {
                0x11, 0x00, 0x33, 0x00, 0x55, 0x00, 0x77, 0x00,
                0x99, 0x00, 0xBB, 0x00, 0xDD, 0x00, 0xFF, 0x01,
            };
            std::memcpy(cpu.sse.xmm[8].u8_, dst, 16);
            cpu.flags.rflags = 0x8D5u;
        },
        true, true);
}

TEST_F(JitParity64Test, NativePcmpeqwMemoryUsesFullEa) {
    constexpr GuestAddr data_address = 0x140015000ull;
    map_tracked_region(data_address, 0x1000);
    ASSERT_TRUE(m_memory->write_u64(data_address,
                                    0x0004000300020001ull).is_ok());
    ASSERT_TRUE(m_memory->write_u64(data_address + 8,
                                    0x0008000700060005ull).is_ok());
    // PCMPEQW XMM8, [RAX] — 66 44 0F 75 00
    check_native_parity({0x66, 0x44, 0x0F, 0x75, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.sse.xmm[8].u16_[0] = 0x0001;
            cpu.sse.xmm[8].u16_[1] = 0xFFFF;
            cpu.sse.xmm[8].u16_[2] = 0x0003;
            cpu.sse.xmm[8].u16_[3] = 0x0000;
            cpu.sse.xmm[8].u16_[4] = 0x0005;
            cpu.sse.xmm[8].u16_[5] = 0x0006;
            cpu.sse.xmm[8].u16_[6] = 0x1111;
            cpu.sse.xmm[8].u16_[7] = 0x0008;
            cpu.flags.rflags = 0x8D5u;
        },
        true, true);
}

TEST_F(JitParity64Test, NativePcmpeqdMemoryUsesFullEa) {
    constexpr GuestAddr data_address = 0x140016000ull;
    map_tracked_region(data_address, 0x1000);
    ASSERT_TRUE(m_memory->write_u64(data_address,
                                    0x2222222211111111ull).is_ok());
    ASSERT_TRUE(m_memory->write_u64(data_address + 8,
                                    0x4444444433333333ull).is_ok());
    // PCMPEQD XMM8, [RAX] — 66 44 0F 76 00
    check_native_parity({0x66, 0x44, 0x0F, 0x76, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.sse.xmm[8].u32_[0] = 0x11111111u;
            cpu.sse.xmm[8].u32_[1] = 0x00000000u;
            cpu.sse.xmm[8].u32_[2] = 0x33333333u;
            cpu.sse.xmm[8].u32_[3] = 0x44444444u;
            cpu.flags.rflags = 0x8D5u;
        },
        true, true);
}

TEST_F(JitParity64Test, NativePackuswbMemoryUsesFullEa) {
    constexpr GuestAddr data_address = 0x140017000ull;
    map_tracked_region(data_address, 0x1000);
    // Source words that saturate unsigned bytes: 0, 0x7F, 0x80, 0xFF, …
    ASSERT_TRUE(m_memory->write_u64(data_address,
                                    0x00FF0080007F0000ull).is_ok());
    ASSERT_TRUE(m_memory->write_u64(data_address + 8,
                                    0x02000100010000FFull).is_ok());
    // PACKUSWB XMM8, [RAX] — 66 44 0F 67 00
    check_native_parity({0x66, 0x44, 0x0F, 0x67, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.sse.xmm[8].u16_[0] = 0x0001;
            cpu.sse.xmm[8].u16_[1] = 0x00FE;
            cpu.sse.xmm[8].u16_[2] = 0x0100;  // saturates to 0xFF
            cpu.sse.xmm[8].u16_[3] = 0xFFFF;  // saturates to 0xFF
            cpu.sse.xmm[8].u16_[4] = 0x0002;
            cpu.sse.xmm[8].u16_[5] = 0x0003;
            cpu.sse.xmm[8].u16_[6] = 0x0004;
            cpu.sse.xmm[8].u16_[7] = 0x0005;
            cpu.flags.rflags = 0x8D5u;
        },
        true, true);
}

// ============================================================================
// Multi-instruction 64-bit sequences
// ============================================================================

TEST_F(JitParity64Test, MultiInstr64Chain) {
    // MOV RAX, 0x100; ADD RAX, RBX; SHL RAX, 4
    u8 code[] = {
        0x48, 0xC7, 0xC0, 0x00, 0x01, 0x00, 0x00,  // MOV RAX, 0x100
        0x48, 0x01, 0xD8,                              // ADD RAX, RBX
        0x48, 0xC1, 0xE0, 0x04,                        // SHL RAX, 4
    };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) { cpu.gpr.set64(RBX, 0x50); });
}

TEST_F(JitParity64Test, MultiInstr64PushPopCalc) {
    // PUSH RAX; XOR RAX, RAX; ADD RAX, RBX; POP RCX
    u8 code[] = {
        0x50,              // PUSH RAX
        0x48, 0x31, 0xC0,  // XOR RAX, RAX
        0x48, 0x01, 0xD8,  // ADD RAX, RBX
        0x59,              // POP RCX
    };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xAAAAAAAABBBBBBBBull);
            cpu.gpr.set64(RBX, 42);
        });
}

TEST_F(JitParity64Test, MulR64MemoryAbove4GiB) {
    constexpr u64 data_addr = 0x100006000ull;
    ASSERT_TRUE(m_memory->map_region(data_addr, 0x1000,
                                    MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(m_memory->write_u64(data_addr, 0x8000000000000003ull).is_ok());

    // MUL QWORD PTR [R8] — 49 F7 20. The checked memory-source path must
    // retain the original RAX value across any host helper used by the load.
    check_parity({0x49, 0xF7, 0x20},
        [=](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFEDCBA9876543210ull);
            cpu.gpr.set64(R8, data_addr);
        });
}

TEST_F(JitParity64Test, MulR64SourceAliasesImplicitOutputs) {
    // MUL RAX; MUL RDX. Each source is read before the architectural output
    // registers are replaced.
    check_parity({0x48, 0xF7, 0xE0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xF000000000000003ull);
            cpu.gpr.set64(RDX, 0x0123456789ABCDEFull);
        });
    check_parity({0x48, 0xF7, 0xE2},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFEDCBA9876543210ull);
            cpu.gpr.set64(RDX, 0x8000000000000005ull);
        });
}
