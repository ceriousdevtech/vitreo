// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): CMOVcc r32,
// NOT/ROL/ROR r32, CQO/CDQ and cookie checks.

#include "jit_parity_64_fixture.hpp"

// ============================================================================
// CMOVcc r32, r/m32 in a 64-bit block.
// Intel SDM Vol. 2: taken writes zero-extended src32; not-taken still
// zero-extends DEST[63:32]. (VITREO_JIT_NO_DELEGATE=1 pins the native path.)
// ============================================================================

TEST_F(JitParity64Test, CmovbR32Taken) {
    // CMOVB EAX, ECX — 0F 42 C1, CF=1 → EAX = ECX (zero-extended)
    check_parity({0x0F, 0x42, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFF11112222ull); // upper must be cleared
            cpu.gpr.set64(RCX, 0x00000000AABBCCDDull);
            cpu.flags.set_cf(true);
        },
        /*check_flags=*/false);
}

TEST_F(JitParity64Test, CmovbR32NotTakenZeroExtends) {
    // CMOVB EAX, ECX — CF=0 → dest[31:0] kept, dest[63:32] cleared.
    check_parity({0x0F, 0x42, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xDEADBEEFCAFEF00Dull);
            cpu.gpr.set64(RCX, 0x0000000012345678ull);
            cpu.flags.set_cf(false);
        },
        false);
}

TEST_F(JitParity64Test, CmovnbR32Taken) {
    // CMOVNB EAX, ECX — 0F 43 C1, CF=0 → taken
    check_parity({0x0F, 0x43, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RCX, 0x000000007EADBEEFull);
            cpu.flags.set_cf(false);
        },
        false);
}

TEST_F(JitParity64Test, CmovzR32Taken) {
    // CMOVZ EAX, ECX — 0F 44 C1, ZF=1 → taken
    check_parity({0x0F, 0x44, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x123456789ABCDEF0ull);
            cpu.gpr.set64(RCX, 0x00000000CAFEBABEull);
            cpu.flags.set_zf(true);
        },
        false);
}

TEST_F(JitParity64Test, CmovzR32NotTaken) {
    // CMOVZ EAX, ECX — ZF=0 → not taken, full RAX preserved
    check_parity({0x0F, 0x44, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x0FEDCBA987654321ull);
            cpu.gpr.set64(RCX, 0x00000000DEADC0DEull);
            cpu.flags.set_zf(false);
        },
        false);
}

TEST_F(JitParity64Test, CmovbR32MemTaken) {
    // CMOVB EAX, [RDX] — 0F 42 02, CF=1 → EAX = [RDX] (zero-extended)
    u8 src[4] = {0x44, 0x33, 0x22, 0x11}; // little-endian 0x11223344
    ASSERT_TRUE(m_memory->write_bytes(0x2000, src, 4));
    check_parity({0x0F, 0x42, 0x02},
        [](CpuState& cpu) {
            cpu.gpr.set64(RDX, 0x2000);
            cpu.gpr.set64(RAX, 0xFFFFFFFF00000000ull);
            cpu.flags.set_cf(true);
        },
        false);
}

TEST_F(JitParity64Test, CmovbR32MemNotTaken) {
    // CMOVB EAX, [RDX] — CF=0 → not taken, full RAX preserved, no mem effect
    u8 src[4] = {0xDD, 0xCC, 0xBB, 0xAA};
    ASSERT_TRUE(m_memory->write_bytes(0x2000, src, 4));
    check_parity({0x0F, 0x42, 0x02},
        [](CpuState& cpu) {
            cpu.gpr.set64(RDX, 0x2000);
            cpu.gpr.set64(RAX, 0x99887766554433FFull);
            cpu.flags.set_cf(false);
        },
        false);
}

TEST_F(JitParity64Test, NotR32) {
    // NOT EAX — F7 D0 (register form is native; result zero-extends to 64)
    check_parity({0xF7, 0xD0},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0xFFFFFFFF0F0F0F0Full); },
        /*check_flags=*/false);
}

TEST_F(JitParity64Test, NotRm32Memory) {
    // NOT dword [RBX] — F7 13. Memory form previously delegated.
    ASSERT_TRUE(m_memory->write_u32(0x2000, 0x0F0F0F0Fu).is_ok());
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0xF7, 0x13},
        [](CpuState& cpu) { cpu.gpr.set64(RBX, 0x2000); },
        /*check_flags=*/false);
}

TEST_F(JitParity64Test, RorRm32MemoryImm) {
    // ROR dword [RBX], 3 — C1 0B 03
    ASSERT_TRUE(m_memory->write_u32(0x2000, 0x12345671u).is_ok());
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0xC1, 0x0B, 0x03},
        [](CpuState& cpu) { cpu.gpr.set64(RBX, 0x2000); });
}

TEST_F(JitParity64Test, RolRm32MemoryImm) {
    // ROL dword [RBX], 3 — C1 03 03
    ASSERT_TRUE(m_memory->write_u32(0x2000, 0x12345671u).is_ok());
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0xC1, 0x03, 0x03},
        [](CpuState& cpu) { cpu.gpr.set64(RBX, 0x2000); });
}

TEST_F(JitParity64Test, RolRm32MemoryBy1) {
    // ROL dword [RBX], 1 — D1 03
    ASSERT_TRUE(m_memory->write_u32(0x2000, 0x80000001u).is_ok());
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0xD1, 0x03},
        [](CpuState& cpu) { cpu.gpr.set64(RBX, 0x2000); });
}

TEST_F(JitParity64Test, RorR32ByCL) {
    // ROR EAX, CL — D3 C8; CL=3. Upper 32 of RAX must zero-extend.
    check_native_parity({0xD3, 0xC8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFF12345671ull);
            cpu.gpr.set64(RCX, 3);
        });
}

TEST_F(JitParity64Test, RolR32ByCL) {
    // ROL EAX, CL — D3 C0; CL=5.
    check_native_parity({0xD3, 0xC0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xAAAAAAAA80000001ull);
            cpu.gpr.set64(RCX, 5);
        });
}

TEST_F(JitParity64Test, RorR32ByCLZero) {
    // Masked count 0 is a complete no-op (dst upper half + all flags).
    check_native_parity({0xD3, 0xC8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xAAAAAAAADEADBEEFull);
            cpu.gpr.set64(RCX, 32);
            cpu.flags.set_cf(true);
            cpu.flags.set_zf(true);
            cpu.flags.set_sf(true);
            cpu.flags.set_of(true);
        });
}

TEST_F(JitParity64Test, RolR32ByCLOneOF) {
    // ROL EAX, CL with CL=1 — OF is defined (CF XOR MSB).
    check_native_parity({0xD3, 0xC0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x40000000ull);
            cpu.gpr.set64(RCX, 1);
        });
}

TEST_F(JitParity64Test, RorRm32ByCLMemory) {
    // ROR dword [RBX], CL — D3 0B
    ASSERT_TRUE(m_memory->write_u32(0x2000, 0x12345671u).is_ok());
    check_native_parity({0xD3, 0x0B},
        [](CpuState& cpu) {
            cpu.gpr.set64(RBX, 0x2000);
            cpu.gpr.set64(RCX, 3);
        });
    auto stored = m_memory->read_u32(0x2000);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0x22468ACEull);
}

TEST_F(JitParity64Test, RolRm32ByCLMemory) {
    // ROL dword [RBX], CL — D3 03
    ASSERT_TRUE(m_memory->write_u32(0x2000, 0x80000001u).is_ok());
    check_native_parity({0xD3, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set64(RBX, 0x2000);
            cpu.gpr.set64(RCX, 1);
        });
    auto stored = m_memory->read_u32(0x2000);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0x00000003u);
}

TEST_F(JitParity64Test, RorR32ByCLPreservesPriorSubZf) {
    // Partial-flag: SUB sets ZF; ROR EAX,CL must not replace that FlagOp.
    check_native_parity(
        {0x29, 0xC9,        // SUB ECX, ECX
         0xD3, 0xC8},       // ROR EAX, CL  (CL=0 after SUB → no-op, but
                            // still exercises the CL path's zero skip)
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x00000007ull);
            cpu.gpr.set64(RCX, 0); // will be zeroed by SUB anyway
            cpu.flags.set_zf(false);
        });
}

TEST_F(JitParity64Test, RorR32ByCLAfterSubNonzero) {
    // SUB ECX,1 leaves ZF=0; ROR EAX,CL (CL=3) updates CF only.
    check_native_parity(
        {0x83, 0xE9, 0x01,  // SUB ECX, 1
         0xD3, 0xC8},       // ROR EAX, CL
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x00000007ull);
            cpu.gpr.set64(RCX, 4); // CL becomes 3 after SUB
            cpu.flags.set_zf(true);
        });
}

TEST_F(JitParity64Test, CqoPositive) {
    // CQO — 48 99: RDX := sign-extend(RAX)
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x48, 0x99},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x7FFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RDX, 0xDEADBEEFCAFEBABEull);
        },
        /*check_flags=*/false);
}

TEST_F(JitParity64Test, CqoNegative) {
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x48, 0x99},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x8000000000000000ull);
            cpu.gpr.set64(RDX, 0);
        },
        /*check_flags=*/false);
}

TEST_F(JitParity64Test, CdqInLongMode) {
    // CDQ — 99: EDX := sign-extend(EAX). RAX is architecturally unchanged.
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity({0x99},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x0000123480000000ull);
            cpu.gpr.set64(RDX, 0x1111111122222222ull);
        },
        /*check_flags=*/false);
}

// 64-bit effective addresses must not wrap through the 32-bit JIT memory
// path. SpotifySetup (image 0x140000000) also VirtualAllocs 256 MiB at
// 0x40000000; truncating the high image address aliases that heap.
TEST_F(JitParity64Test, Ror64ClPreservesPriorSubZf) {
    // Intel SDM Vol. 2 ROR: CF/OF only. A following consumer must still see
    // ZF from the SUB. Seed ZF=1 so a FlagOp::Ror64 replacement would leak it.
    check_native_parity(
        {0x48, 0x29, 0xC8,        // SUB RAX, RCX
         0x48, 0xD3, 0xCA},       // ROR RDX, CL
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 5);
            cpu.gpr.set64(RCX, 1);
            cpu.gpr.set64(RDX, 0x8000000000000001ull);
            cpu.flags.set_zf(true);
            cpu.flags.set_cf(false);
        });
}

TEST_F(JitParity64Test, EncodePointerXorRor64) {
    // MSVC cookie decode: RSI = ROR(cookie XOR [RDX], CL). Same shape as
    // SpotifySetup's pointer-guard store at 0x140009245 / 0x14000924e.
    const u64 cookie = 0x00006e7ed037ad9dull;
    const u64 encoded = 0x1122334455667788ull;
    ASSERT_TRUE(m_memory->write_bytes(
        0x2000, reinterpret_cast<const u8*>(&encoded), 8).is_ok());
    check_native_parity(
        {0x49, 0x8B, 0xF2,        // MOV RSI, R10
         0x48, 0x33, 0x32,        // XOR RSI, [RDX]
         0x83, 0xE1, 0x3F,        // AND ECX, 0x3F
         0x48, 0xD3, 0xCE},       // ROR RSI, CL
        [cookie](CpuState& cpu) {
            cpu.gpr.set64(10, cookie); // R10
            cpu.gpr.set64(RSI, 0xDEADBEEFCAFEBABEull);
            cpu.gpr.set64(RDX, 0x2000);
            cpu.gpr.set64(RCX, cookie); // CL overwritten by AND
        });
}

TEST_F(JitParity64Test, StosqAdvancesRdi) {
    u8 buf[16] = {};
    ASSERT_TRUE(m_memory->write_bytes(0x2000, buf, sizeof(buf)).is_ok());
    check_native_parity(
        {0x48, 0xAB},             // STOSQ
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x0102030405060708ull);
            cpu.gpr.set64(RDI, 0x2000);
            cpu.flags.set_df(false);
        },
        /*check_flags=*/false);
    u64 word = 0;
    ASSERT_TRUE(m_memory->read_bytes(
        0x2000, reinterpret_cast<u8*>(&word), 8).is_ok());
    EXPECT_EQ(word, 0x0102030405060708ull);
}

TEST_F(JitParity64Test, Cmov32MemUsesFull64BitAddress) {
    constexpr GuestAddr kHigh = 0x140001000ull;
    constexpr GuestAddr kAlias = 0x40001000ull;
    ASSERT_TRUE(m_memory->map_region(
        kHigh, 0x4000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(m_memory->map_region(
        kAlias, 0x4000, MemoryProtection::ReadWrite).is_ok());
    const u32 high_val = 0xA1B2C3D4u;
    const u32 alias_val = 0x11223344u;
    ASSERT_TRUE(m_memory->write_bytes(
        kHigh, reinterpret_cast<const u8*>(&high_val), 4).is_ok());
    ASSERT_TRUE(m_memory->write_bytes(
        kAlias, reinterpret_cast<const u8*>(&alias_val), 4).is_ok());

    // CMOVNZ EAX, [RCX]  (0F 45 01) — taken, so EAX must be the high dword.
    check_native_parity({0x0F, 0x45, 0x01},
        [](CpuState& cpu) {
            cpu.gpr.set64(RCX, kHigh);
            cpu.gpr.set64(RAX, 0xCAFEF00D99887766ull);
            cpu.flags.set_zf(false);
        },
        /*check_flags=*/false);
}

TEST_F(JitParity64Test, SecurityCheckCookieRolTestCx) {
    // MSVC x64 __security_check_cookie after a matching CMP:
    //   rol rcx, 10h / test cx, 0xffff
    // Cookies keep bits [63:48] zero so CX is 0 after the rotate. A 32-bit
    // ROL would leave CX nonzero and take the GS-failure path.
    constexpr u64 cookie = 0x00001AE9CFA71D22ull;
    check_native_parity({
            0x48, 0xC1, 0xC1, 0x10,       // ROL RCX, 16
            0x66, 0xF7, 0xC1, 0xFF, 0xFF, // TEST CX, 0xFFFF
        },
        [](CpuState& cpu) { cpu.gpr.set64(RCX, cookie); },
        /*check_flags=*/true);

    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    const u8 code[] = {0x48, 0xC1, 0xC1, 0x10, 0x66, 0xF7, 0xC1, 0xFF, 0xFF};
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = build_block(code, sizeof(code));
    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set64(RCX, cookie);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu), JitExecResult::Continue);
    cpu.materialize_lazy_flags();
    EXPECT_EQ(cpu.gpr.get64(RCX), 0x1AE9CFA71D220000ull);
    EXPECT_TRUE(cpu.flags.zf());
}

TEST_F(JitParity64Test, SecurityCheckCookieXorRspRoundtrip) {
    // Prologue/epilogue: cookie ^= RSP; store; load; ^= RSP; CMP cookie.
    constexpr GuestAddr kCookie = 0x2000;
    constexpr GuestAddr kSlot = 0x2100;
    constexpr u64 cookie = 0x00001AE9CFA71D22ull;
    ASSERT_TRUE(m_memory->write_bytes(
        kCookie, reinterpret_cast<const u8*>(&cookie), 8).is_ok());
    check_native_parity({
            0x48, 0x8B, 0x02,             // MOV RAX, [RDX]
            0x48, 0x33, 0xC4,             // XOR RAX, RSP
            0x48, 0x89, 0x01,             // MOV [RCX], RAX
            0x48, 0x8B, 0x01,             // MOV RAX, [RCX]
            0x48, 0x33, 0xC4,             // XOR RAX, RSP
            0x48, 0x3B, 0x02,             // CMP RAX, [RDX]
        },
        [](CpuState& cpu) {
            cpu.gpr.set64(RDX, kCookie);
            cpu.gpr.set64(RCX, kSlot);
            cpu.gpr.set64(RSP, 0x7FFDFE10ull);
        },
        /*check_flags=*/true);
}

TEST_F(JitParity64Test, MovdqaMemUsesFull64BitAddress) {
    // CRT vector init does MOVDQA xmm0, [rip+disp] from a 0x14000xxxx
    // all-ones template. A 32-bit EA aliases that onto a 0x40000xxxx heap
    // mapping and leaves the real .bss slots as zero.
    constexpr GuestAddr kHigh = 0x140001000ull;
    constexpr GuestAddr kAlias = 0x40001000ull;
    ASSERT_TRUE(m_memory->map_region(
        kHigh, 0x4000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(m_memory->map_region(
        kAlias, 0x4000, MemoryProtection::ReadWrite).is_ok());
    const u8 high_bytes[16] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    const u8 alias_bytes[16] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00,
    };
    ASSERT_TRUE(m_memory->write_bytes(kHigh, high_bytes, 16).is_ok());
    ASSERT_TRUE(m_memory->write_bytes(kAlias, alias_bytes, 16).is_ok());
    ASSERT_TRUE(m_memory->write_bytes(kHigh + 0x1000, alias_bytes, 16).is_ok());
    ASSERT_TRUE(m_memory->write_bytes(kAlias + 0x1000, alias_bytes, 16).is_ok());

    // MOVDQA XMM0, [RAX]; MOVDQA [RCX], XMM0
    check_native_parity({0x66, 0x0F, 0x6F, 0x00, 0x66, 0x0F, 0x7F, 0x01},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, kHigh);
            cpu.gpr.set64(RCX, kHigh + 0x1000);
        },
        /*check_flags=*/false, /*check_xmm=*/true);

    u8 out[16] = {};
    u8 alias_out[16] = {};
    ASSERT_TRUE(m_memory->read_bytes(kHigh + 0x1000, out, 16).is_ok());
    ASSERT_TRUE(m_memory->read_bytes(kAlias + 0x1000, alias_out, 16).is_ok());
    EXPECT_EQ(std::memcmp(out, high_bytes, 16), 0);
    EXPECT_EQ(std::memcmp(alias_out, alias_bytes, 16), 0);
}
