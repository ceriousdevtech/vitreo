// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): SHLD/SHRD,
// BT/BTS/BTR/BTC, LZCNT/TZCNT and CVTSI2SD.

#include "jit_parity_64_fixture.hpp"

// ============================================================================
// SHLD / SHRD r/m64 native emit parity
// ============================================================================

TEST_F(JitParity64Test, Shld64ImmCount0PreservesDestAndFlags) {
    check_native_parity({0x48, 0x0F, 0xA4, 0xD0, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x0123456789ABCDEFull);
            cpu.gpr.set64(RDX, 0xFEDCBA9876543210ull);
            cpu.flags.set_cf(true);
            cpu.flags.set_of(true);
            cpu.flags.set_zf(true);
            cpu.flags.set_sf(false);
        });
}

TEST_F(JitParity64Test, Shld64ImmCount1Overflow) {
    check_native_parity({0x48, 0x0F, 0xA4, 0xD0, 0x01},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x4000000000000000ull);
            cpu.gpr.set64(RDX, 0x8000000000000000ull);
        });
}

TEST_F(JitParity64Test, Shld64ImmMultiBit) {
    check_native_parity({0x48, 0x0F, 0xA4, 0xD0, 0x04},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x0123456789ABCDEFull);
            cpu.gpr.set64(RDX, 0xFEDCBA9876543210ull);
            cpu.flags.set_of(true);
        });
}

TEST_F(JitParity64Test, Shrd64ImmCount1Overflow) {
    check_native_parity({0x48, 0x0F, 0xAC, 0xD0, 0x01},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x8000000000000001ull);
            cpu.gpr.set64(RDX, 0);
        });
}

TEST_F(JitParity64Test, Shrd64ImmMultiBit) {
    check_native_parity({0x48, 0x0F, 0xAC, 0xD0, 0x1E},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x0123456789ABCDEFull);
            cpu.gpr.set64(RDX, 0xFEDCBA9876543210ull);
        });
}

TEST_F(JitParity64Test, Shld64ByCl) {
    check_native_parity({0x48, 0x0F, 0xA5, 0xD0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x1111222233334444ull);
            cpu.gpr.set64(RDX, 0x5555666677778888ull);
            cpu.gpr.set64(RCX, 12);
        });
}

TEST_F(JitParity64Test, Shrd64ByClCount0) {
    check_native_parity({0x48, 0x0F, 0xAD, 0xD0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x0123456789ABCDEFull);
            cpu.gpr.set64(RDX, 0xFEDCBA9876543210ull);
            cpu.gpr.set64(RCX, 0);
            cpu.flags.set_cf(true);
            cpu.flags.set_zf(false);
        });
}

TEST_F(JitParity64Test, Shift64MemoryByClKeepsCountAcrossCheckedLoad) {
    m_compare_memory = true;
    for (const u8 modrm : {u8{0x23}, u8{0x2b}, u8{0x3b}}) {
        for (const u64 count : {0ull, 1ull, 7ull, 31ull, 63ull, 64ull, 65ull, 127ull}) {
            SCOPED_TRACE(static_cast<unsigned>(modrm));
            SCOPED_TRACE(count);
            // Keep every probe cold and avoid the repeated-SMC demotion guard.
            m_jit = std::make_unique<JitCompiler>(*m_memory);
            ASSERT_TRUE(m_memory->write_u64(0x2000, 0x8123456789abcdefull));
            check_native_parity({0x48, 0xd3, modrm}, [count](CpuState& cpu) {
                cpu.gpr.set64(RBX, 0x2000);
                cpu.gpr.set64(RCX, count);
            });
        }
    }
}

TEST_F(JitParity64Test, Shld64MemoryDest) {
    ASSERT_TRUE(m_memory->write_u64(0x2000, 0x0123456789ABCDEFull));
    check_native_parity({0x48, 0x0F, 0xA4, 0x13, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RBX, 0x2000);
            cpu.gpr.set64(RDX, 0xFEDCBA9876543210ull);
        });
}

// ============================================================================
// BT / BTS / BTR / BTC r/m64 + LZCNT/TZCNT + CVTSI2SD r64
// ============================================================================

TEST_F(JitParity64Test, NativeBtR64RegisterSetsCarry) {
    // BT RAX, RCX — 48 0F A3 C8
    check_native_parity({0x48, 0x0F, 0xA3, 0xC8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x0000000000000080ull);
            cpu.gpr.set64(RCX, 7);
            cpu.flags.set_cf(false);
            cpu.flags.set_zf(true);
        });
}

TEST_F(JitParity64Test, NativeBtcR64RegisterComplementsBit) {
    // BTC RAX, RCX — 48 0F BB C8
    check_native_parity({0x48, 0x0F, 0xBB, 0xC8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x8000000000000001ull);
            cpu.gpr.set64(RCX, 63);
            cpu.flags.set_cf(false);
        });
}

TEST_F(JitParity64Test, NativeBtsR64MemorySetsBitAcrossQword) {
    // BTS [RAX], RCX with index 70 → bit 6 of byte at base+8.
    constexpr GuestAddr data_address = 0x14002D000ull;
    map_tracked_region(data_address, 0x1000);
    ASSERT_TRUE(m_memory->write_u64(data_address, 0));
    ASSERT_TRUE(m_memory->write_u64(data_address + 8, 0));
    check_native_parity({0x48, 0x0F, 0xAB, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.gpr.set64(RCX, 70);
            cpu.flags.set_cf(true);
        });
    auto lo = m_memory->read_u64(data_address);
    auto hi = m_memory->read_u64(data_address + 8);
    ASSERT_TRUE(lo.is_ok());
    ASSERT_TRUE(hi.is_ok());
    EXPECT_EQ(lo.value(), 0ull);
    EXPECT_EQ(hi.value(), 1ull << 6);
}

TEST_F(JitParity64Test, NativeBtrR64Imm8ClearsBit) {
    // BTR RAX, 5 — 48 0F BA F0 05
    check_native_parity({0x48, 0x0F, 0xBA, 0xF0, 0x05},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x000000000000003Full);
            cpu.flags.set_cf(false);
        });
}

TEST_F(JitParity64Test, NativeBtcR64Imm8Memory) {
    // BTC qword ptr [RAX], 0 — 48 0F BA 38 00
    ASSERT_TRUE(m_memory->write_u64(0x2100, 0x1ull));
    check_native_parity({0x48, 0x0F, 0xBA, 0x38, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x2100);
            cpu.flags.set_cf(false);
        });
    auto stored = m_memory->read_u64(0x2100);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0ull);
}

TEST_F(JitParity64Test, NativeLzcntR64ZeroSetsCarry) {
    // LZCNT RAX, RCX — F3 48 0F BD C1
    check_native_parity({0xF3, 0x48, 0x0F, 0xBD, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xDEADBEEFull);
            cpu.gpr.set64(RCX, 0);
            cpu.flags.set_cf(false);
            cpu.flags.set_zf(true);
        });
}

TEST_F(JitParity64Test, NativeTzcntR64Memory) {
    // TZCNT RDX, [RAX] — F3 48 0F BC 10
    ASSERT_TRUE(m_memory->write_u64(0x2200, 0x0000000100000000ull));
    check_native_parity({0xF3, 0x48, 0x0F, 0xBC, 0x10},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x2200);
            cpu.gpr.set64(RDX, 0xFFFFull);
            cpu.flags.set_cf(true);
            cpu.flags.set_zf(true);
        });
}

TEST_F(JitParity64Test, NativeCvtsi2sdR64PreservesHighQword) {
    // CVTSI2SD XMM8, RCX — F2 4C 0F 2A C1
    check_native_parity({0xF2, 0x4C, 0x0F, 0x2A, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RCX, static_cast<u64>(static_cast<i64>(-42)));
            cpu.sse.xmm[8].u64_[0] = 0xAAAAAAAAAAAAAAAAULL;
            cpu.sse.xmm[8].u64_[1] = 0xBBBBBBBBBBBBBBBBULL;
            cpu.flags.rflags = 0x8D5u;
        },
        true, true);
}

TEST_F(JitParity64Test, VexIntegerSourceDoesNotAliasSameNumberedXmm) {
    // VCVTSI2SD xmm0, xmm1, eax. EAX and XMM0 have the same
    // decoder register index, but are distinct architectural registers.
    check_native_parity({0xC5, 0xF3, 0x2A, 0xC0},
        [](CpuState& cpu) {
            cpu.gpr.set32(RAX, static_cast<u32>(-42));
            cpu.sse.xmm[0].u64_[0] = 0xAAAAAAAAAAAAAAAAull;
            cpu.sse.xmm[0].u64_[1] = 0xBBBBBBBBBBBBBBBBull;
            cpu.sse.xmm[1].u64_[1] = 0x123456789ABCDEF0ull;
            cpu.avx.ymm_high[0].u64_[0] = ~0ull;
            cpu.avx.ymm_high[0].u64_[1] = ~0ull;
        }, true, true);
}

TEST_F(JitParity64Test, NativeVmovntdq256StoresBothLanes) {
    // VMOVNTDQ [rax], ymm0: VEX.256.66.0F E7 /r.
    check_native_parity({0xC5, 0xFD, 0xE7, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x2200);
            cpu.sse.xmm[0].u64_[0] = 0x0123456789ABCDEFull;
            cpu.sse.xmm[0].u64_[1] = 0xFEDCBA9876543210ull;
            cpu.avx.ymm_high[0].u64_[0] = 0x1122334455667788ull;
            cpu.avx.ymm_high[0].u64_[1] = 0x8877665544332211ull;
        }, true, true);
}

TEST_F(JitParity64Test, VexInteger64SourceDoesNotAliasSameNumberedXmm) {
    // VCVTSI2SD xmm0, xmm1, rax (VEX.W=1).
    check_native_parity({0xC4, 0xE1, 0xF3, 0x2A, 0xC0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, static_cast<u64>(static_cast<i64>(-42)));
            cpu.sse.xmm[1].u64_[1] = 0x123456789ABCDEF0ull;
            cpu.avx.ymm_high[0].u64_[0] = ~0ull;
            cpu.avx.ymm_high[0].u64_[1] = ~0ull;
        }, true, true);
}

TEST_F(JitParity64Test, NativeVmovntdq256RejectsSixteenByteOnlyAlignment) {
    const u8 code[] = {0xC5, 0xFD, 0xE7, 0x00};
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_EQ(compiled->native_insns, 1u);
    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set64(RAX, 0x2210); // 16-byte aligned, not 32-byte aligned.
    cpu.sse.xmm[0].u64_[0] = ~0ull;
    cpu.avx.ymm_high[0].u64_[0] = ~0ull;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    EXPECT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    u8 out[32]{};
    ASSERT_TRUE(m_memory->read_bytes(0x2210, out, sizeof(out)));
    for (u8 byte : out) EXPECT_EQ(byte, 0);
}
