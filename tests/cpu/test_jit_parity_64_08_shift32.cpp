// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): 32-bit-operand
// shifts and IMUL r32.

#include "jit_parity_64_fixture.hpp"

// ============================================================================
// 32-bit-operand shifts inside a 64-bit block (native codegen added for the
// Steam-extraction hot path: SHR/SHL/SAR r/m32 by imm8/1/CL). Each must match
// the interpreter on result, flags, AND zero-extension of the upper 32 bits.
// ============================================================================

TEST_F(JitParity64Test, ShrR32Imm8) {
    // SHR EAX, 5 — C1 E8 05; upper 32 of RAX must be cleared.
    check_parity({0xC1, 0xE8, 0x05},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0xFFFFFFFFDEADBEE1ull); });
}

TEST_F(JitParity64Test, ShlR32Imm8) {
    // SHL EAX, 5 — C1 E0 05
    check_parity({0xC1, 0xE0, 0x05},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0xFFFFFFFF80001234ull); });
}

TEST_F(JitParity64Test, SarR32Imm8) {
    // SAR EAX, 5 — C1 F8 05 (sign-extends within 32 bits, then zero-extends)
    check_parity({0xC1, 0xF8, 0x05},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x0000000080000000ull); });
}

TEST_F(JitParity64Test, ShlR32Imm1OF) {
    // SHL EAX, 1 — D1 E0 (single-bit shift sets OF)
    check_parity({0xD1, 0xE0},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x40000000ull); });
}

TEST_F(JitParity64Test, ShrR32Imm1) {
    // SHR EAX, 1 — D1 E8 (CF = bit shifted out)
    check_parity({0xD1, 0xE8},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x00000003ull); });
}

TEST_F(JitParity64Test, ShrR32ByCL) {
    // SHR EAX, CL — D3 E8; CL=7
    check_parity({0xD3, 0xE8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFF12345678ull);
            cpu.gpr.set64(RCX, 7);
        });
}

TEST_F(JitParity64Test, ShlR32ByCL) {
    // SHL EAX, CL — D3 E0; CL=12
    check_parity({0xD3, 0xE0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x000ABCDEull);
            cpu.gpr.set64(RCX, 12);
        });
}

TEST_F(JitParity64Test, ShrR32ByCLZero) {
    // SHR EAX, CL with CL=0 — flags are unchanged, but the 32-bit destination
    // write still happens and zero-extends into bits 63:32 (Intel SDM Vol. 1
    // 3.4.1.1; confirmed on real x86-64: RAX 0xAAAAAAAADEADBEEF -> 0xDEADBEEF).
    check_parity({0xD3, 0xE8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xAAAAAAAADEADBEEFull);
            cpu.gpr.set64(RCX, 0);
        });
}

// Every zero-count 32-bit shift, rotate and double-shift zero-extends the
// destination register. Measured on real x86-64 hardware, not inferred: the
// JIT rotate paths used to treat a zero count as a complete no-op and leave
// bits 63:32 intact.
TEST_F(JitParity64Test, RorR32ByCLZeroZeroExtends) {
    // ROR EAX, CL with CL=0 — D3 C8
    check_parity({0xD3, 0xC8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xAAAAAAAADEADBEEFull);
            cpu.gpr.set64(RCX, 0);
        });
}

TEST_F(JitParity64Test, RolR32ByCLZeroZeroExtends) {
    // ROL EAX, CL with CL=0 — D3 C0
    check_parity({0xD3, 0xC0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xAAAAAAAADEADBEEFull);
            cpu.gpr.set64(RCX, 0);
        });
}

TEST_F(JitParity64Test, RorR32ByImmZeroZeroExtends) {
    // ROR EAX, 0 — C1 C8 00
    check_parity({0xC1, 0xC8, 0x00},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0xAAAAAAAADEADBEEFull); });
}

TEST_F(JitParity64Test, RolR32ByImmZeroZeroExtends) {
    // ROL EAX, 0 — C1 C0 00
    check_parity({0xC1, 0xC0, 0x00},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0xAAAAAAAADEADBEEFull); });
}

TEST_F(JitParity64Test, ShlR32ByCLZeroZeroExtends) {
    // SHL EAX, CL with CL=0 — D3 E0
    check_parity({0xD3, 0xE0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xAAAAAAAADEADBEEFull);
            cpu.gpr.set64(RCX, 0);
        });
}

TEST_F(JitParity64Test, SarR32ByCLZeroZeroExtends) {
    // SAR EAX, CL with CL=0 — D3 F8
    check_parity({0xD3, 0xF8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xAAAAAAAADEADBEEFull);
            cpu.gpr.set64(RCX, 0);
        });
}

TEST_F(JitParity64Test, ShrR32ByCLMasked32ZeroExtends) {
    // SHR EAX, CL with CL=32 — x86 masks the count to 5 bits, giving 0, so
    // flags are untouched and the destination still zero-extends.
    check_parity({0xD3, 0xE8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFABCD1234ull);
            cpu.gpr.set64(RCX, 32);
        });
}

TEST_F(JitParity64Test, ShrR8dImm8RexB) {
    // SHR R8D, 5 — 41 C1 E8 05 (REX.B extends to R8D)
    check_parity({0x41, 0xC1, 0xE8, 0x05},
        [](CpuState& cpu) { cpu.gpr.set64(R8, 0xFFFFFFFFCAFE00FFull); });
}

TEST_F(JitParity64Test, NativeShrRm8OneHighByteRegister) {
    // SHR AH,1 — D0 EC. Preserve every other byte of RAX while deriving
    // CF/OF from the original 0x81 high byte.
    check_native_parity({0xD0, 0xEC},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x11223344556681AAull);
            cpu.flags.rflags = 0x2u;
        });
}

TEST_F(JitParity64Test, NativeShrRm8OneMemoryUsesFullEa) {
    // Keep the high-address byte zero so interpreter and JIT can both apply
    // the RMW idempotently. A truncating JIT would instead alter the mapped
    // low alias at 0x3003, which the parity snapshot detects.
    constexpr GuestAddr data_address = 0x100003000ull;
    ASSERT_TRUE(m_memory->map_region(data_address, 0x1000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u8(data_address + 3, 0).is_ok());
    ASSERT_TRUE(m_memory->write_u8(0x3003, 0x80u).is_ok());
    // SHR byte ptr [RAX+3],1 — D0 68 03.
    check_native_parity({0xD0, 0x68, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.flags.rflags = 0x8D5u;
        });
}

TEST_F(JitParity64Test, NativeShrRm32Imm8Memory) {
    // SHR dword ptr [RAX], 5 — C1 /5. This was delegated in long mode and is
    // a dominant Steam extraction fallback.
    constexpr GuestAddr data_address = 0x2000;
    ASSERT_TRUE(m_memory->write_u32(data_address, 0x8000003Fu));
    check_native_parity({0xC1, 0x28, 0x05},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, data_address); });
    auto stored = m_memory->read_u32(data_address);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0x04000001u);
}

TEST_F(JitParity64Test, NativeShrRm32ByCLMemory) {
    // SHR dword ptr [RAX], CL — D3 /5. The checked load must complete before
    // CL is captured because a high-address miss may call a host helper.
    constexpr GuestAddr data_address = 0x2000;
    ASSERT_TRUE(m_memory->write_u32(data_address, 0xF000000Fu));
    check_native_parity({0xD3, 0x28},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.gpr.set64(RCX, 4);
        });
    auto stored = m_memory->read_u32(data_address);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0x0F000000u);
}

TEST_F(JitParity64Test, NativeShlRm32ByCLMemory) {
    // SHL dword ptr [RAX], CL — D3 /4.
    constexpr GuestAddr data_address = 0x2000;
    ASSERT_TRUE(m_memory->write_u32(data_address, 0x0000000Fu));
    check_native_parity({0xD3, 0x20},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.gpr.set64(RCX, 4);
        });
    auto stored = m_memory->read_u32(data_address);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0x000000F0u);
}

TEST_F(JitParity64Test, NativeSarRm32ByCLMemory) {
    // SAR dword ptr [RAX], CL — D3 /7.
    constexpr GuestAddr data_address = 0x2000;
    ASSERT_TRUE(m_memory->write_u32(data_address, 0x80000000u));
    check_native_parity({0xD3, 0x38},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.gpr.set64(RCX, 4);
        });
    auto stored = m_memory->read_u32(data_address);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0xF8000000u);
}

TEST_F(JitParity64Test, NativeShrR16Imm8) {
    // SHR BX, 1 — 66 D1 /5. The result preserves RBX's upper 48 bits and the
    // lazy state records the shifted-out carry.
    check_native_parity({0x66, 0xD1, 0xEB},
        [](CpuState& cpu) {
            cpu.gpr.set64(RBX, 0xAAAABBBBCCCC8003ull);
            cpu.flags.set_of(false);
        });
}

TEST_F(JitParity64Test, NativeShrRm16Imm8Memory) {
    // SHR word ptr [RAX], 5 — 66 C1 /5.
    constexpr GuestAddr data_address = 0x2000;
    ASSERT_TRUE(m_memory->write_u16(data_address, 0x803Fu));
    check_native_parity({0x66, 0xC1, 0x28, 0x05},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, data_address); });
    auto stored = m_memory->read_u16(data_address);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0x0401u);
}

TEST_F(JitParity64Test, NativeShrR16ByCLZero) {
    // SHR BX, CL with a masked zero count is a complete no-op.
    check_native_parity({0x66, 0xD3, 0xEB},
        [](CpuState& cpu) {
            cpu.gpr.set64(RBX, 0x1111222233338003ull);
            cpu.gpr.set64(RCX, 32);
            cpu.flags.set_cf(true);
            cpu.flags.set_of(true);
        });
}

// ============================================================================
// IMUL r32, r/m32 (and 3-operand imm forms) inside a 64-bit block.
// ============================================================================

TEST_F(JitParity64Test, ImulR32R32) {
    // IMUL EAX, EBX — 0F AF C3
    check_parity({0x0F, 0xAF, 0xC3},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x000000000001E240ull);
            cpu.gpr.set64(RBX, 0xFFFFFFFFFFFFFFFBull); // EBX = -5
        });
}

TEST_F(JitParity64Test, ImulR32R32Overflow) {
    // IMUL EAX, EBX where product overflows 32 bits — sets CF/OF
    check_parity({0x0F, 0xAF, 0xC3},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x12345678ull);
            cpu.gpr.set64(RBX, 0x12345678ull);
        });
}

TEST_F(JitParity64Test, ImulR32R32Imm32) {
    // IMUL EAX, EBX, 0x1234 — 69 C3 34 12 00 00
    check_parity({0x69, 0xC3, 0x34, 0x12, 0x00, 0x00},
        [](CpuState& cpu) { cpu.gpr.set64(RBX, 0x000FEDCBull); });
}

TEST_F(JitParity64Test, ImulR32R32Imm8) {
    // IMUL EAX, EBX, 7 — 6B C3 07
    check_parity({0x6B, 0xC3, 0x07},
        [](CpuState& cpu) { cpu.gpr.set64(RBX, 0x20000001ull); });
}
