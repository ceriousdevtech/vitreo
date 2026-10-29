// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): string ops,
// SIMD memory sources and PEXTR/PINSR.

#include "jit_parity_64_fixture.hpp"

TEST_F(JitParity64Test, NativeRepStosqFillsQwords) {
    // REP STOSQ — F3 48 AB. AddressSize=64 uses RCX count and RDI pointer.
    u8 buf[64] = {};
    ASSERT_TRUE(m_memory->write_bytes(0x2000, buf, sizeof(buf)).is_ok());
    check_native_parity(
        {0xF3, 0x48, 0xAB},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x0102030405060708ull);
            cpu.gpr.set64(RDI, 0x2000);
            cpu.gpr.set64(RCX, 4);
            cpu.flags.set_df(false);
        },
        /*check_flags=*/false);
    for (u64 i = 0; i < 4; ++i) {
        u64 word = 0;
        ASSERT_TRUE(m_memory->read_bytes(
            0x2000 + i * 8, reinterpret_cast<u8*>(&word), 8).is_ok());
        EXPECT_EQ(word, 0x0102030405060708ull);
    }
}

TEST_F(JitParity64Test, NativeRepStosqDfBackward) {
    // REP STOSQ with DF=1 writes toward lower addresses.
    u8 buf[64] = {};
    ASSERT_TRUE(m_memory->write_bytes(0x2000, buf, sizeof(buf)).is_ok());
    check_native_parity(
        {0xF3, 0x48, 0xAB},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xAABBCCDDEEFF0011ull);
            cpu.gpr.set64(RDI, 0x2018);
            cpu.gpr.set64(RCX, 3);
            cpu.flags.set_df(true);
        },
        /*check_flags=*/false);
}

TEST_F(JitParity64Test, NativeMovsqCopiesQword) {
    // MOVSQ — 48 A5
    ASSERT_TRUE(m_memory->write_u64(0x2000, 0x1122334455667788ull));
    ASSERT_TRUE(m_memory->write_u64(0x2100, 0));
    check_native_parity(
        {0x48, 0xA5},
        [](CpuState& cpu) {
            cpu.gpr.set64(RSI, 0x2000);
            cpu.gpr.set64(RDI, 0x2100);
            cpu.flags.set_df(false);
        },
        /*check_flags=*/false);
    auto word = m_memory->read_u64(0x2100);
    ASSERT_TRUE(word.is_ok());
    EXPECT_EQ(word.value(), 0x1122334455667788ull);
}

TEST_F(JitParity64Test, NativeRepMovsqCopiesQwords) {
    // REP MOVSQ — F3 48 A5. AddressSize=64 uses RCX count and RSI/RDI.
    u8 src[32];
    for (u64 i = 0; i < 4; ++i) {
        const u64 word = 0x1000000000000000ull + i;
        std::memcpy(src + i * 8, &word, 8);
    }
    u8 dst[32] = {};
    ASSERT_TRUE(m_memory->write_bytes(0x2000, src, sizeof(src)).is_ok());
    ASSERT_TRUE(m_memory->write_bytes(0x2200, dst, sizeof(dst)).is_ok());
    check_native_parity(
        {0xF3, 0x48, 0xA5},
        [](CpuState& cpu) {
            cpu.gpr.set64(RSI, 0x2000);
            cpu.gpr.set64(RDI, 0x2200);
            cpu.gpr.set64(RCX, 4);
            cpu.flags.set_df(false);
        },
        /*check_flags=*/false);
    for (u64 i = 0; i < 4; ++i) {
        u64 word = 0;
        ASSERT_TRUE(m_memory->read_bytes(
            0x2200 + i * 8, reinterpret_cast<u8*>(&word), 8).is_ok());
        EXPECT_EQ(word, 0x1000000000000000ull + i);
    }
}

TEST_F(JitParity64Test, NativeRepMovsqDfBackward) {
    // REP MOVSQ with DF=1 copies toward lower addresses.
    ASSERT_TRUE(m_memory->write_u64(0x2010, 0xAAAABBBBCCCCDDDDull));
    ASSERT_TRUE(m_memory->write_u64(0x2008, 0x1111222233334444ull));
    ASSERT_TRUE(m_memory->write_u64(0x2110, 0));
    ASSERT_TRUE(m_memory->write_u64(0x2108, 0));
    check_native_parity(
        {0xF3, 0x48, 0xA5},
        [](CpuState& cpu) {
            cpu.gpr.set64(RSI, 0x2010);
            cpu.gpr.set64(RDI, 0x2110);
            cpu.gpr.set64(RCX, 2);
            cpu.flags.set_df(true);
        },
        /*check_flags=*/false);
}

TEST_F(JitParity64Test, NativeCmpsqSetsFlags) {
    // CMPSQ — 48 A7
    ASSERT_TRUE(m_memory->write_u64(0x2000, 0x1122334455667788ull));
    ASSERT_TRUE(m_memory->write_u64(0x2100, 0x1122334455667788ull));
    check_native_parity(
        {0x48, 0xA7},
        [](CpuState& cpu) {
            cpu.gpr.set64(RSI, 0x2000);
            cpu.gpr.set64(RDI, 0x2100);
            cpu.flags.set_df(false);
            cpu.flags.rflags = 0x246u;
        });
}

TEST_F(JitParity64Test, NativeRepeCmpsqStopsOnMismatch) {
    // REPE CMPSQ — F3 48 A7
    ASSERT_TRUE(m_memory->write_u64(0x2000, 0x1));
    ASSERT_TRUE(m_memory->write_u64(0x2008, 0x2));
    ASSERT_TRUE(m_memory->write_u64(0x2010, 0x3));
    ASSERT_TRUE(m_memory->write_u64(0x2100, 0x1));
    ASSERT_TRUE(m_memory->write_u64(0x2108, 0x22));
    ASSERT_TRUE(m_memory->write_u64(0x2110, 0x3));
    check_native_parity(
        {0xF3, 0x48, 0xA7},
        [](CpuState& cpu) {
            cpu.gpr.set64(RSI, 0x2000);
            cpu.gpr.set64(RDI, 0x2100);
            cpu.gpr.set64(RCX, 3);
            cpu.flags.set_df(false);
            cpu.flags.set_zf(true);
        });
}

TEST_F(JitParity64Test, NativeRepneCmpsqStopsOnMatch) {
    // REPNE CMPSQ — F2 48 A7
    ASSERT_TRUE(m_memory->write_u64(0x2000, 0xA));
    ASSERT_TRUE(m_memory->write_u64(0x2008, 0xB));
    ASSERT_TRUE(m_memory->write_u64(0x2100, 0x1));
    ASSERT_TRUE(m_memory->write_u64(0x2108, 0xB));
    check_native_parity(
        {0xF2, 0x48, 0xA7},
        [](CpuState& cpu) {
            cpu.gpr.set64(RSI, 0x2000);
            cpu.gpr.set64(RDI, 0x2100);
            cpu.gpr.set64(RCX, 2);
            cpu.flags.set_df(false);
            cpu.flags.set_zf(false);
        });
}

TEST_F(JitParity64Test, NativeScasqSetsFlags) {
    // SCASQ — 48 AF
    ASSERT_TRUE(m_memory->write_u64(0x2000, 0xCAFEBABEDEADBEEFull));
    check_native_parity(
        {0x48, 0xAF},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xCAFEBABEDEADBEEFull);
            cpu.gpr.set64(RDI, 0x2000);
            cpu.flags.set_df(false);
            cpu.flags.rflags = 0x246u;
        });
}

TEST_F(JitParity64Test, NativeRepeScasqStopsOnMismatch) {
    // REPE SCASQ — F3 48 AF
    ASSERT_TRUE(m_memory->write_u64(0x2000, 0x55));
    ASSERT_TRUE(m_memory->write_u64(0x2008, 0x55));
    ASSERT_TRUE(m_memory->write_u64(0x2010, 0x66));
    check_native_parity(
        {0xF3, 0x48, 0xAF},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x55);
            cpu.gpr.set64(RDI, 0x2000);
            cpu.gpr.set64(RCX, 3);
            cpu.flags.set_df(false);
            cpu.flags.set_zf(true);
        });
}

TEST_F(JitParity64Test, NativeRepneScasqStopsOnMatch) {
    // REPNE SCASQ — F2 48 AF
    ASSERT_TRUE(m_memory->write_u64(0x2000, 0x11));
    ASSERT_TRUE(m_memory->write_u64(0x2008, 0x22));
    ASSERT_TRUE(m_memory->write_u64(0x2010, 0x33));
    check_native_parity(
        {0xF2, 0x48, 0xAF},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x22);
            cpu.gpr.set64(RDI, 0x2000);
            cpu.gpr.set64(RCX, 3);
            cpu.flags.set_df(false);
            cpu.flags.set_zf(false);
        });
}

TEST_F(JitParity64Test, NativePshufbMemoryMask) {
    // PSHUFB XMM0, [RAX] — 66 0F 38 00 00
    u8 mask[16] = {
        15, 14, 13, 12, 0x1B, 0x2A, 0x39, 0x48,
        7, 6, 5, 4, 0x80, 0x8F, 1, 0,
    };
    ASSERT_TRUE(m_memory->write_bytes(0x2000, mask, sizeof(mask)).is_ok());
    check_native_parity({0x66, 0x0F, 0x38, 0x00, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x2000);
            for (u8 i = 0; i < 16; ++i)
                cpu.sse.xmm[0].u8_[i] = static_cast<u8>(0x40u + i);
            cpu.flags.rflags = 0x8D5u;
        },
        true, true);
}

TEST_F(JitParity64Test, NativePshufdMemorySource) {
    // PSHUFD XMM0, [RCX], 0x1B — 66 0F 70 01 1B
    u32 dwords[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
    ASSERT_TRUE(m_memory->write_bytes(
        0x2000, reinterpret_cast<const u8*>(dwords), sizeof(dwords)).is_ok());
    check_native_parity({0x66, 0x0F, 0x70, 0x01, 0x1B},
        [](CpuState& cpu) {
            cpu.gpr.set64(RCX, 0x2000);
            cpu.sse.xmm[0].u64_[0] = 0;
            cpu.sse.xmm[0].u64_[1] = 0;
        },
        false, true);
}

TEST_F(JitParity64Test, NativePadddMemorySource) {
    // PADDD XMM0, [RDX] — 66 0F FE 02
    u32 src[4] = {1u, 2u, 3u, 4u};
    ASSERT_TRUE(m_memory->write_bytes(
        0x2000, reinterpret_cast<const u8*>(src), sizeof(src)).is_ok());
    check_native_parity({0x66, 0x0F, 0xFE, 0x02},
        [](CpuState& cpu) {
            cpu.gpr.set64(RDX, 0x2000);
            cpu.sse.xmm[0].u32_[0] = 10;
            cpu.sse.xmm[0].u32_[1] = 20;
            cpu.sse.xmm[0].u32_[2] = 30;
            cpu.sse.xmm[0].u32_[3] = 40;
        },
        false, true);
}

TEST_F(JitParity64Test, NativePalignrMemorySource) {
    // PALIGNR XMM0, [RBX], 5 — 66 0F 3A 0F 03 05
    u8 src[16];
    for (u8 i = 0; i < 16; ++i) src[i] = static_cast<u8>(0xA0u + i);
    ASSERT_TRUE(m_memory->write_bytes(0x2000, src, sizeof(src)).is_ok());
    check_native_parity({0x66, 0x0F, 0x3A, 0x0F, 0x03, 0x05},
        [](CpuState& cpu) {
            cpu.gpr.set64(RBX, 0x2000);
            for (u8 i = 0; i < 16; ++i)
                cpu.sse.xmm[0].u8_[i] = static_cast<u8>(0x10u + i);
        },
        false, true);
}

// A byte store into a read-only page must still fault: the inline store path
// performs no permission check, so correctness depends on such a region never
// being published. Both engines must refuse the write.
TEST_F(JitParity64Test, NativeByteStoreToReadOnlyMemoryDoesNotWrite) {
    constexpr GuestAddr ro_address = 0x14002C000ull;
    map_tracked_region(ro_address, 0x1000);
    ASSERT_TRUE(m_memory->write_u8(ro_address, 0x11));
    ASSERT_TRUE(m_memory->protect_region(ro_address, 0x1000,
                                         MemoryProtection::Read));

    const u8 code[] = {0x88, 0x08};  // MOV [RAX], CL
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    CpuState cpu;
    cpu.init_long_mode();
    cpu.rip = 0x1000;
    cpu.gpr.set64(RSP, 0x8000);
    cpu.gpr.set64(RAX, ro_address);
    cpu.gpr.set64(RCX, 0x99);
    (void)test::execute_like_dispatcher(*m_jit, *compiled, cpu);

    auto value = m_memory->read_u8(ro_address);
    ASSERT_TRUE(value.is_ok());
    EXPECT_EQ(value.value(), 0x11)
        << "a store reached a read-only page through the inline fast path";
}

// Register-destination counterparts of the read-modify-write cases above.
// Their memory-destination forms diverge on CF/OF; these isolate whether the
// defect is in the flag computation itself or only in the memory path.
TEST_F(JitParity64Test, NativeShrByteRegisterByOneMatchesInterpreter) {
    // SHR CL, 1 — D0 E9
    check_native_parity({0xD0, 0xE9},
        [](CpuState& cpu) { cpu.gpr.set64(RCX, 0x81); });
}

TEST_F(JitParity64Test, NativeSub8RegisterDestinationMatchesInterpreter) {
    // SUB CL, 0x20 — 80 E9 20
    check_native_parity({0x80, 0xE9, 0x20},
        [](CpuState& cpu) { cpu.gpr.set64(RCX, 0x10); });
}

TEST_F(JitParity64Test, NativeIncByteRegisterPreservesCarry) {
    // INC CL — FE C1
    check_native_parity({0xFE, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RCX, 0xFF);
            cpu.flags.set_cf(true);
        });
}

TEST_F(JitParity64Test, NativeAdd32RegisterDestinationMatchesInterpreter) {
    // ADD EAX, ECX — 01 C8
    check_native_parity({0x01, 0xC8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x0000FFFFull);
            cpu.gpr.set64(RCX, 0x00000001ull);
        });
}

// PEXTRQ/PINSRQ are the REX.W forms of PEXTRD/PINSRD and address a 64-bit
// lane, so a decoder that ignores REX.W silently reads or writes the wrong
// half of the register. The CRT's vectorized std::count over bytes ends in
// PEXTRQ to pick up the high half of a PSADBW fold, which is where a missing
// 64-bit form turns into a wrong answer rather than a fault.
TEST_F(JitParity64Test, NativePextrqExtractsHighLane) {
    // PEXTRQ RDX, XMM1, 1 — 66 48 0F 3A 16 CA 01
    check_parity({0x66, 0x48, 0x0F, 0x3A, 0x16, 0xCA, 0x01},
        [](CpuState& cpu) {
            cpu.sse.xmm[1].u64_[0] = 0x1122334455667788ull;
            cpu.sse.xmm[1].u64_[1] = 0x99AABBCCDDEEFF00ull;
            cpu.gpr.set64(RDX, 0);
        },
        false, true);
}

TEST_F(JitParity64Test, NativePextrqExtractsLowLane) {
    // PEXTRQ RDX, XMM1, 0 — 66 48 0F 3A 16 CA 00
    check_parity({0x66, 0x48, 0x0F, 0x3A, 0x16, 0xCA, 0x00},
        [](CpuState& cpu) {
            cpu.sse.xmm[1].u64_[0] = 0x1122334455667788ull;
            cpu.sse.xmm[1].u64_[1] = 0x99AABBCCDDEEFF00ull;
            cpu.gpr.set64(RDX, 0);
        },
        false, true);
}

TEST_F(JitParity64Test, NativePinsrqWritesSelectedLane) {
    // PINSRQ XMM1, RDX, 1 — 66 48 0F 3A 22 CA 01
    check_parity({0x66, 0x48, 0x0F, 0x3A, 0x22, 0xCA, 0x01},
        [](CpuState& cpu) {
            cpu.sse.xmm[1].u64_[0] = 0x1122334455667788ull;
            cpu.sse.xmm[1].u64_[1] = 0x99AABBCCDDEEFF00ull;
            cpu.gpr.set64(RDX, 0xDEADBEEFCAFEBABEull);
        },
        false, true);
}

TEST_F(JitParity64Test, NativePextrdStillTruncatesToDword) {
    // PEXTRD EDX, XMM1, 1 — 66 0F 3A 16 CA 01 (no REX.W)
    check_parity({0x66, 0x0F, 0x3A, 0x16, 0xCA, 0x01},
        [](CpuState& cpu) {
            cpu.sse.xmm[1].u64_[0] = 0x1122334455667788ull;
            cpu.sse.xmm[1].u64_[1] = 0x99AABBCCDDEEFF00ull;
            cpu.gpr.set64(RDX, 0xFFFFFFFFFFFFFFFFull);
        },
        false, true);
}
