// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): SSE.

#include "jit_parity_64_fixture.hpp"

// ============================================================================
// 64-bit SSE parity
// ============================================================================

TEST_F(JitParity64Test, MovdqaXmm) {
    // MOVDQA XMM0, XMM1 — 66 0F 6F C1
    check_parity({0x66, 0x0F, 0x6F, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[1].u64_[0] = 0xDEADBEEFCAFEBABEull;
            cpu.sse.xmm[1].u64_[1] = 0x1234567890ABCDEFull;
        },
        false, true);
}

TEST_F(JitParity64Test, PadddXmm) {
    // PADDD XMM0, XMM1 — 66 0F FE C1
    check_parity({0x66, 0x0F, 0xFE, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[0].u32_[0] = 0xFFFFFFFF;
            cpu.sse.xmm[0].u32_[1] = 100;
            cpu.sse.xmm[0].u32_[2] = 0;
            cpu.sse.xmm[0].u32_[3] = 0x7FFFFFFF;
            cpu.sse.xmm[1].u32_[0] = 1;
            cpu.sse.xmm[1].u32_[1] = 200;
            cpu.sse.xmm[1].u32_[2] = 300;
            cpu.sse.xmm[1].u32_[3] = 1;
        },
        false, true);
}

TEST_F(JitParity64Test, NativeMovsdRegisterPreservesUpperLane) {
    // MOVSD XMM8,XMM9 — F2 45 0F 10 C1.
    check_native_parity({0xF2, 0x45, 0x0F, 0x10, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[8].u64_[0] = 0xAAAAAAAAAAAAAAAAull;
            cpu.sse.xmm[8].u64_[1] = 0xBBBBBBBBBBBBBBBBull;
            cpu.sse.xmm[9].u64_[0] = 0x0123456789ABCDEFull;
            cpu.sse.xmm[9].u64_[1] = 0xFEDCBA9876543210ull;
            cpu.flags.rflags = 0x8D5u;
        },
        true, true);
}

TEST_F(JitParity64Test, NativeMovsdMemoryLoadUsesFullEaAndClearsUpperLane) {
    constexpr GuestAddr data_address = 0x14000D000ull;
    ASSERT_TRUE(m_memory->map_region(data_address, 0x1000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u64(data_address + 8,
                                    0x0123456789ABCDEFull).is_ok());
    // MOVSD XMM8,[RAX+8] — F2 44 0F 10 40 08.
    check_native_parity({0xF2, 0x44, 0x0F, 0x10, 0x40, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.sse.xmm[8].u64_[0] = 0xAAAAAAAAAAAAAAAAull;
            cpu.sse.xmm[8].u64_[1] = 0xBBBBBBBBBBBBBBBBull;
            cpu.flags.rflags = 0x8D5u;
        },
        true, true);
}

TEST_F(JitParity64Test, NativeMovsdMemoryStoreWritesExactlyEightBytes) {
    ASSERT_TRUE(m_memory->write_u64(0x3008, 0xAAAAAAAAAAAAAAAAull).is_ok());
    ASSERT_TRUE(m_memory->write_u64(0x3010, 0xBBBBBBBBBBBBBBBBull).is_ok());
    // MOVSD [RAX+8],XMM9 — F2 44 0F 11 48 08.
    check_native_parity({0xF2, 0x44, 0x0F, 0x11, 0x48, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.sse.xmm[9].u64_[0] = 0x0123456789ABCDEFull;
            cpu.sse.xmm[9].u64_[1] = 0xFEDCBA9876543210ull;
            cpu.flags.rflags = 0x8D5u;
        },
        true, true);
}

TEST_F(JitParity64Test, NativePclmulqdqRegister) {
    // PCLMULQDQ XMM8, XMM9, 0x11 — REX.R/B verifies the upper XMM bank.
    check_native_parity({0x66, 0x45, 0x0F, 0x3A, 0x44, 0xC1, 0x11},
        [](CpuState& cpu) {
            cpu.sse.xmm[8].u64_[0] = 0x0123456789ABCDEFull;
            cpu.sse.xmm[8].u64_[1] = 0xFEDCBA9876543210ull;
            cpu.sse.xmm[9].u64_[0] = 0x1111111111111111ull;
            cpu.sse.xmm[9].u64_[1] = 0x8000000000000003ull;
        },
        false, true);
}

TEST_F(JitParity64Test, NativeXorpsAndPxorRegisters) {
    // XORPS XMM8,XMM9; PXOR XMM10,XMM11. REX.R/B verifies upper XMMs.
    check_native_parity({
            0x45, 0x0F, 0x57, 0xC1,
            0x66, 0x45, 0x0F, 0xEF, 0xD3,
        },
        [](CpuState& cpu) {
            cpu.sse.xmm[8].u64_[0] = 0xFFFF0000FFFF0000ull;
            cpu.sse.xmm[8].u64_[1] = 0xAAAAAAAA55555555ull;
            cpu.sse.xmm[9].u64_[0] = 0x00FFFF0000FFFF00ull;
            cpu.sse.xmm[9].u64_[1] = 0x123456789ABCDEF0ull;
            cpu.sse.xmm[10].u64_[0] = 0x0123456789ABCDEFull;
            cpu.sse.xmm[10].u64_[1] = 0xFEDCBA9876543210ull;
            cpu.sse.xmm[11].u64_[0] = 0x1111111111111111ull;
            cpu.sse.xmm[11].u64_[1] = 0x2222222222222222ull;
        },
        false, true);
}

TEST_F(JitParity64Test, NativeXorpsMemoryUsesFullEa) {
    // XORPS XMM8,[RAX+16] — 44 0F 57 40 10. The operation is a raw 128-bit
    // XOR and the memory source uses the complete long-mode EA.
    //
    // The displacement is 16, not 8, because legacy SSE raises #GP on a
    // misaligned 128-bit memory operand -- for every one of these forms, not
    // only the MOVAPS/MOVDQA copies. Measured on silicon:
    // tools/oracle_probe/sse_align_probe.c and its recorded output. This test
    // used [RAX+8] and called it "unaligned"; it was asserting behaviour no
    // real CPU has, and it is what kept the check out of the emitter.
    constexpr GuestAddr data_address = 0x14000B000ull;
    ASSERT_TRUE(m_memory->map_region(data_address, 0x1000,
                                     MemoryProtection::ReadWrite));
    const u64 source[2] = {
        0x00FFFF0000FFFF00ull,
        0x123456789ABCDEF0ull,
    };
    ASSERT_TRUE(m_memory->write_bytes(
        data_address + 16, reinterpret_cast<const Byte*>(source),
        sizeof(source)));
    check_native_parity({0x44, 0x0F, 0x57, 0x40, 0x10},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.sse.xmm[8].u64_[0] = 0xFFFF0000FFFF0000ull;
            cpu.sse.xmm[8].u64_[1] = 0xAAAAAAAA55555555ull;
            cpu.flags.rflags = 0x8D5u;
        },
        true, true);
}

TEST_F(JitParity64Test, NativeMovdFromUpperXmmToUpperGpr) {
    // MOVD R9D,XMM8 — both REX.R and REX.B are active. The r32 write must
    // clear R9[63:32].
    check_native_parity({0x66, 0x45, 0x0F, 0x7E, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(static_cast<u8>(RegIndex::R9),
                          0xFFFFFFFF00000000ull);
            cpu.sse.xmm[8].u32_[0] = 0x89ABCDEFu;
            cpu.sse.xmm[8].u32_[1] = 0x01234567u;
        },
        false, true);
}

TEST_F(JitParity64Test, NativeMovdFromXmmToMemoryUsesFullEa) {
    // MOVD [RAX],XMM8 — store exactly four low bytes through a 64-bit EA.
    check_native_parity({0x66, 0x44, 0x0F, 0x7E, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x2043);
            cpu.sse.xmm[8].u32_[0] = 0x78563412u;
        },
        false, true);
    u8 out[6] = {};
    ASSERT_TRUE(m_memory->read_bytes(0x2042, out, sizeof(out)).is_ok());
    EXPECT_EQ(out[0], 0);
    EXPECT_EQ(out[1], 0x12);
    EXPECT_EQ(out[2], 0x34);
    EXPECT_EQ(out[3], 0x56);
    EXPECT_EQ(out[4], 0x78);
    EXPECT_EQ(out[5], 0);
}

TEST_F(JitParity64Test, NativeMovdToXmmClearsUpperBits) {
    // MOVD XMM8,R9D — 66 45 0F 6E C1. Low dword from GPR; bits 127:32 cleared.
    check_native_parity({0x66, 0x45, 0x0F, 0x6E, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(static_cast<u8>(RegIndex::R9),
                          0xFFFFFFFF89ABCDEFull);
            cpu.sse.xmm[8].u64_[0] = 0xAAAAAAAAAAAAAAAAULL;
            cpu.sse.xmm[8].u64_[1] = 0xBBBBBBBBBBBBBBBBULL;
            cpu.flags.rflags = 0x8D5u;
        },
        true, true);
}

TEST_F(JitParity64Test, NativeMovdMemoryToXmmClearsUpperBits) {
    constexpr GuestAddr data_address = 0x140011000ull;
    ASSERT_TRUE(m_memory->map_region(data_address, 0x1000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u32(data_address + 4, 0x11223344u).is_ok());
    // MOVD XMM8,[RAX+4] — 66 44 0F 6E 40 04.
    check_native_parity({0x66, 0x44, 0x0F, 0x6E, 0x40, 0x04},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.sse.xmm[8].u64_[0] = 0xAAAAAAAAAAAAAAAAULL;
            cpu.sse.xmm[8].u64_[1] = 0xBBBBBBBBBBBBBBBBULL;
            cpu.flags.rflags = 0x8D5u;
        },
        true, true);
}

TEST_F(JitParity64Test, NativeMovqXmmFromXmmClearsUpperLane) {
    // MOVQ XMM8,XMM9 — F3 45 0F 7E C1.
    check_native_parity({0xF3, 0x45, 0x0F, 0x7E, 0xC1},
        [](CpuState& cpu) {
            cpu.sse.xmm[8].u64_[0] = 0xAAAAAAAAAAAAAAAAULL;
            cpu.sse.xmm[8].u64_[1] = 0xBBBBBBBBBBBBBBBBULL;
            cpu.sse.xmm[9].u64_[0] = 0x0123456789ABCDEFULL;
            cpu.sse.xmm[9].u64_[1] = 0xFEDCBA9876543210ULL;
            cpu.flags.rflags = 0x8D5u;
        },
        true, true);
}

TEST_F(JitParity64Test, NativeMovqXmmFromMemoryClearsUpperLane) {
    constexpr GuestAddr data_address = 0x140012000ull;
    ASSERT_TRUE(m_memory->map_region(data_address, 0x1000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u64(data_address + 8,
                                    0x0123456789ABCDEFULL).is_ok());
    // MOVQ XMM8,[RAX+8] — F3 44 0F 7E 40 08.
    check_native_parity({0xF3, 0x44, 0x0F, 0x7E, 0x40, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.sse.xmm[8].u64_[0] = 0xAAAAAAAAAAAAAAAAULL;
            cpu.sse.xmm[8].u64_[1] = 0xBBBBBBBBBBBBBBBBULL;
            cpu.flags.rflags = 0x8D5u;
        },
        true, true);
}

TEST_F(JitParity64Test, NativeMovqMemoryFromXmmWritesExactlyEightBytes) {
    ASSERT_TRUE(m_memory->write_u64(0x3108, 0xAAAAAAAAAAAAAAAAULL).is_ok());
    ASSERT_TRUE(m_memory->write_u64(0x3110, 0xBBBBBBBBBBBBBBBBULL).is_ok());
    // MOVQ [RAX+8],XMM9 — 66 44 0F D6 48 08.
    check_native_parity({0x66, 0x44, 0x0F, 0xD6, 0x48, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3100);
            cpu.sse.xmm[9].u64_[0] = 0x0123456789ABCDEFULL;
            cpu.sse.xmm[9].u64_[1] = 0xFEDCBA9876543210ULL;
            cpu.flags.rflags = 0x8D5u;
        },
        true, true);
    auto lo = m_memory->read_u64(0x3108);
    auto hi = m_memory->read_u64(0x3110);
    ASSERT_TRUE(lo.is_ok());
    ASSERT_TRUE(hi.is_ok());
    EXPECT_EQ(lo.value(), 0x0123456789ABCDEFULL);
    EXPECT_EQ(hi.value(), 0xBBBBBBBBBBBBBBBBULL);
}

TEST_F(JitParity64Test, NativePunpckldqMemoryInterleavesLowDwords) {
    constexpr GuestAddr data_address = 0x140013000ull;
    ASSERT_TRUE(m_memory->map_region(data_address, 0x1000,
                                     MemoryProtection::ReadWrite));
    // Memory src low qword = {0x11111111, 0x22222222}; high ignored by LDQ.
    ASSERT_TRUE(m_memory->write_u64(data_address,
                                    0x2222222211111111ULL).is_ok());
    ASSERT_TRUE(m_memory->write_u64(data_address + 8,
                                    0x4444444433333333ULL).is_ok());
    // PUNPCKLDQ XMM8,[RAX] — 66 44 0F 62 00.
    check_native_parity({0x66, 0x44, 0x0F, 0x62, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.sse.xmm[8].u32_[0] = 0xAAAAAAAAu;
            cpu.sse.xmm[8].u32_[1] = 0xBBBBBBBBu;
            cpu.sse.xmm[8].u32_[2] = 0xCCCCCCCCu;
            cpu.sse.xmm[8].u32_[3] = 0xDDDDDDDDu;
            cpu.flags.rflags = 0x8D5u;
        },
        true, true);
}

TEST_F(JitParity64Test, NativePsrldqByteShiftAndSaturatingCount) {
    // PSRLDQ XMM8,7; PSRLDQ XMM9,16. The first exercises byte ordering and
    // the second verifies Intel's count>15 all-zero result.
    check_native_parity({
            0x66, 0x41, 0x0F, 0x73, 0xD8, 0x07,
            0x66, 0x41, 0x0F, 0x73, 0xD9, 0x10,
        },
        [](CpuState& cpu) {
            for (u8 i = 0; i < 16; ++i) {
                cpu.sse.xmm[8].u8_[i] = static_cast<u8>(0x10u + i);
                cpu.sse.xmm[9].u8_[i] = static_cast<u8>(0x80u + i);
            }
        },
        false, true);
}

TEST_F(JitParity64Test, NativePshufbRegisterMaskAndZeroing) {
    // PSHUFB XMM8,XMM9. Exercise the upper XMM bank, reversed selection,
    // ignored mask bits 4-6, and bit-7 zeroing without touching EFLAGS.
    check_native_parity({0x66, 0x45, 0x0F, 0x38, 0x00, 0xC1},
        [](CpuState& cpu) {
            for (u8 i = 0; i < 16; ++i)
                cpu.sse.xmm[8].u8_[i] = static_cast<u8>(0x40u + i);
            const u8 mask[16] = {
                15, 14, 13, 12, 0x1B, 0x2A, 0x39, 0x48,
                7, 6, 5, 4, 0x80, 0x8F, 1, 0,
            };
            std::memcpy(cpu.sse.xmm[9].u8_, mask, sizeof(mask));
            cpu.flags.rflags = 0x8D5u;
        },
        true, true);
}

TEST_F(JitParity64Test, NativePmovmskbUpperRegistersAndBitOrder) {
    // PMOVMSKB R9D,XMM10 — 66 45 0F D7 CA. Alternating and edge MSBs verify
    // byte-to-mask bit order; the r32 destination must clear R9[63:32].
    check_native_parity({0x66, 0x45, 0x0F, 0xD7, 0xCA},
        [](CpuState& cpu) {
            cpu.gpr.set64(static_cast<u8>(RegIndex::R9),
                          0xFFFFFFFFAAAAAAAAull);
            const u8 bytes[16] = {
                0x80, 0x7F, 0xFF, 0x00, 0x81, 0x01, 0xFE, 0x02,
                0x03, 0xFC, 0x04, 0xF8, 0x10, 0x90, 0x40, 0xC0,
            };
            std::memcpy(cpu.sse.xmm[10].u8_, bytes, sizeof(bytes));
            cpu.flags.rflags = 0x8D5u;
        },
        true, true);
}

TEST_F(JitParity64Test, NativePshufbAliasedSourceSnapshotsDestination) {
    // PSHUFB XMM10,XMM10. Both the lookup table and indices must be loaded
    // before the destructive destination write.
    check_native_parity({0x66, 0x45, 0x0F, 0x38, 0x00, 0xD2},
        [](CpuState& cpu) {
            for (u8 i = 0; i < 16; ++i)
                cpu.sse.xmm[10].u8_[i] = static_cast<u8>(15u - i);
        },
        false, true);
}

TEST_F(JitParity64Test, NativeSha1RegisterSequence) {
    // Exercise every SHA-1 extension form used by a complete long-mode SHA-1
    // round sequence. Varying data catches lane-order and immediate packing.
    check_native_parity({
            0x45, 0x0F, 0x38, 0xC8, 0xC1,        // SHA1NEXTE XMM8, XMM9
            0x45, 0x0F, 0x38, 0xC9, 0xD3,        // SHA1MSG1 XMM10, XMM11
            0x45, 0x0F, 0x38, 0xCA, 0xE5,        // SHA1MSG2 XMM12, XMM13
            0x45, 0x0F, 0x3A, 0xCC, 0xF7, 0x03,  // SHA1RNDS4 XMM14,XMM15,3
        },
        [](CpuState& cpu) {
            for (u8 reg = 8; reg < 16; ++reg) {
                for (u8 lane = 0; lane < 4; ++lane) {
                    cpu.sse.xmm[reg].u32_[lane] =
                        0x10203040u * (reg - 7u) + 0x01010101u * lane;
                }
            }
        },
        false, true);
}

TEST_F(JitParity64Test, NativeMulThenAdcCarriesHighProduct) {
    // MUL RBX; ADC RCX,0. A non-zero high half sets MUL.CF, which the
    // following ADC must consume before replacing the arithmetic flags.
    check_native_parity({
            0x48, 0xF7, 0xE3,
            0x48, 0x83, 0xD1, 0x00,
        },
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RBX, 2);
            cpu.gpr.set64(RCX, 5);
            cpu.flags.rflags = 0x202;
        },
        true, false);
}
