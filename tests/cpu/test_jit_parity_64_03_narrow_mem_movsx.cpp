// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): 16-bit memory
// paths, MOVSXD and PUSH/POP.

#include "jit_parity_64_fixture.hpp"

// ============================================================================
// Native 16-bit MOV memory paths
//
// These force the interpreter delegate off, proving that full-64-bit effective
// addresses and 16-bit partial-register semantics are handled by native x64
// stencils. The helper calls retain GuestMemory protection/fault behavior.
// ============================================================================

TEST_F(JitParity64Test, NativeMovR16FromMem) {
    // MOV BX, [RAX] — 66 8B 18.  Loads 16 bits into BX; upper 48 bits of RBX
    // must be preserved (no zero-extension for a 16-bit destination).
    constexpr GuestAddr data_address = 0x140002000ull;
    ASSERT_TRUE(m_memory->map_region(data_address, 0x1000,
                                     MemoryProtection::ReadWrite));
    u8 val[2] = {0x34, 0x12};  // little-endian 0x1234
    ASSERT_TRUE(m_memory->write_bytes(data_address, val, sizeof(val)));
    check_native_parity({0x66, 0x8B, 0x18},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.gpr.set64(RBX, 0xAAAABBBBCCCCDDDDull);
        },
        false);
}

TEST_F(JitParity64Test, NativeMovR16InMixedBlockPreservesFlags) {
    // TEST RAX,RAX (sets ZF/SF/PF) ; MOV DX,[RCX] (native helper load).
    // The MOV must leave the TEST's flags intact.
    //   TEST RAX, RAX — 48 85 C0 ; MOV DX, [RCX] — 66 8B 11
    u8 val[2] = {0x78, 0x56};  // 0x5678
    ASSERT_TRUE(m_memory->write_bytes(0x2000, val, sizeof(val)));
    check_native_parity({0x48, 0x85, 0xC0, 0x66, 0x8B, 0x11},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0);   // TEST -> ZF=1
            cpu.gpr.set64(RCX, 0x2000);
            cpu.gpr.set64(RDX, 0x1111222233334444ull);
        });
}

TEST_F(JitParity64Test, NativeMovzxR32Rm16LowCacheSurvivesStackReseed) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x1000,
                                  MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(0x20000, 0x1000,
                                  MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(0x30000, 0x1000,
                                  MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.write_u16(0x20000, 0x5A3C));

    const u8 code[] = {0x0F, 0xB7, 0x01};  // MOVZX EAX, word ptr [RCX]
    ASSERT_TRUE(memory.write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = build_block(code, sizeof(code));

    ScopedEnvironmentVariable diag("VITREO_JIT_READ16_DIAG", "1");
    testing::internal::CaptureStderr();
    {
        JitCompiler jit(memory);
        CompiledBlock* compiled = jit.compile(block);
        ASSERT_NE(compiled, nullptr);

        for (int iteration = 0; iteration < 2; ++iteration) {
            CpuState cpu;
            cpu.init_long_mode();
            cpu.rip = 0x1000;
            cpu.gpr.set64(RSP, 0x30800);
            cpu.gpr.set64(RCX, 0x20000);
            EXPECT_EQ(test::execute_like_dispatcher(jit, *compiled, cpu), JitExecResult::Continue);
            EXPECT_EQ(cpu.gpr.rax, 0x5A3Cu);
        }
    }
    const std::string diagnostics = testing::internal::GetCapturedStderr();

    // The first execution resolves the data region. Dispatcher entry seeds
    // the legacy stack slot again before the second execution, but the x64
    // cache must retain the low data region and avoid a second checked read.
    EXPECT_NE(diagnostics.find(
                  "[jit-read16-diag] calls=1 low=1 high=0 "
                  "x86=0 x64=1 unknown_mode=0"),
              std::string::npos)
        << diagnostics;
}

TEST_F(JitParity64Test, NativeMovzxR32Rm16HighMemory) {
    constexpr GuestAddr data_address = 0x140004000ull;
    ASSERT_TRUE(m_memory->map_region(data_address, 0x1000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u16(data_address, 0xD27B));
    check_native_parity({0x0F, 0xB7, 0x01},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RCX, data_address);
        },
        false);
}

TEST_F(JitParity64Test, NativeMovR16ToMem) {
    // MOV [RAX], BX — 66 89 18.
    check_native_parity({0x66, 0x89, 0x18},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x2000);
            cpu.gpr.set64(RBX, 0xAAAABBBBCCCC5678ull);
        },
        false);
    auto stored = m_memory->read_u16(0x2000);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0x5678);
}

TEST_F(JitParity64Test, NativeMovR16ToHighMemUsesFullAddress) {
    // MOV [RAX], BX — the low-cache guard must route this PE32+-range
    // address through the checked full-width helper without truncation.
    constexpr GuestAddr data_address = 0x140004000ull;
    ASSERT_TRUE(m_memory->map_region(data_address, 0x1000,
                                     MemoryProtection::ReadWrite));
    check_native_parity({0x66, 0x89, 0x18},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.gpr.set64(RBX, 0xAAAABBBBCCCC2468ull);
        },
        false);
    auto stored = m_memory->read_u16(data_address);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0x2468);
}

TEST_F(JitParity64Test, NativeMovRm16Imm16ToMem) {
    // MOV word ptr [RAX], 0xBEEF — 66 C7 00 EF BE.
    check_native_parity({0x66, 0xC7, 0x00, 0xEF, 0xBE},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x2000); },
        false);
    auto stored = m_memory->read_u16(0x2000);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0xBEEF);
}

TEST_F(JitParity64Test, NativeAddR16FromHighMemory) {
    // ADD BX, [RAX] — 66 03 18. The source is above 4 GB, so truncating the
    // long-mode effective address would fault or read the wrong region.
    constexpr GuestAddr data_address = 0x140003000ull;
    ASSERT_TRUE(m_memory->map_region(data_address, 0x1000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u16(data_address, 0x0002));
    check_native_parity({0x66, 0x03, 0x18},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.gpr.set64(RBX, 0xAAAABBBBCCCCFFFFull);
        });
}

TEST_F(JitParity64Test, NativeAddRm16R16ToMem) {
    // ADD [RAX], BX — 66 01 18.
    ASSERT_TRUE(m_memory->write_u16(0x2000, 0x7FFF));
    check_native_parity({0x66, 0x01, 0x18},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x2000);
            cpu.gpr.set64(RBX, 1);
        });
    auto stored = m_memory->read_u16(0x2000);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0x8000);
}

TEST_F(JitParity64Test, NativeSubRm16Imm8FromMem) {
    // SUB word ptr [RAX], -1 — 66 83 28 FF (equivalent to +1 modulo 16 bits).
    ASSERT_TRUE(m_memory->write_u16(0x2000, 0xFFFF));
    check_native_parity({0x66, 0x83, 0x28, 0xFF},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x2000); });
    auto stored = m_memory->read_u16(0x2000);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0x0000);
}

TEST_F(JitParity64Test, NativeLeaR32UsesFullEaThenZeroExtends) {
    // LEA EAX, [RBX + RCX*4 + 0x20] — 8D 44 8B 20.
    check_native_parity({0x8D, 0x44, 0x8B, 0x20},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RBX, 0x0000000140000000ull);
            cpu.gpr.set64(RCX, 3);
        },
        false);
}

TEST_F(JitParity64Test, NativeIncR32PreservesCarry) {
    // INC EAX — FF C0. Result zero-extends, while CF remains set.
    check_native_parity({0xFF, 0xC0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xAAAAAAAAFFFFFFFFull);
            cpu.flags.set_cf(true);
        });
}

TEST_F(JitParity64Test, NativeDecR32PreservesCarry) {
    // DEC EAX — FF C8.
    check_native_parity({0xFF, 0xC8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xBBBBBBBB80000000ull);
            cpu.flags.set_cf(false);
        });
}

TEST_F(JitParity64Test, NativeNegR32) {
    // NEG EAX — F7 D8.
    check_native_parity({0xF7, 0xD8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xCCCCCCCC80000000ull);
        });
}

TEST_F(JitParity64Test, NativeIncRm32Memory) {
    // INC dword ptr [RAX] — FF 00.
    constexpr GuestAddr data_address = 0x2000;
    ASSERT_TRUE(m_memory->write_u32(data_address, 0x7FFFFFFF));
    check_native_parity({0xFF, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.flags.set_cf(true);
        });
    auto stored = m_memory->read_u32(data_address);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0x80000000u);
}

TEST_F(JitParity64Test, NativeNegRm32HighMemory) {
    // NEG dword ptr [RAX] — F7 18. This exercises both full-width narrow
    // access fallbacks and the architectural zero-minus-value flags.
    constexpr GuestAddr data_address = 0x140005000ull;
    ASSERT_TRUE(m_memory->map_region(data_address, 0x1000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u32(data_address, 0x80000000u));
    check_native_parity({0xF7, 0x18},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, data_address); });
    auto stored = m_memory->read_u32(data_address);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0x80000000u);
}

TEST_F(JitParity64Test, NativeSbbRm32Imm8ConsumesCarryAndZeroExtends) {
    // SBB EAX,2 — 83 D8 02. Incoming CF participates in the subtraction and
    // the architectural r32 write clears RAX[63:32].
    check_native_parity({0x83, 0xD8, 0x02},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xAAAABBBB00000005ull);
            cpu.flags.set_cf(true);
        });
}

TEST_F(JitParity64Test, NativeSbbRm32Imm8MemoryRmw) {
    // SBB dword ptr [RAX+4],127 — 83 58 04 7F. Exercise checked memory RMW
    // and preservation of the incoming borrow. Full-width addressing is
    // covered independently by the memory-source case below.
    constexpr GuestAddr data_address = 0x2040;
    ASSERT_TRUE(m_memory->write_u32(data_address + 4, 0x80000000u));
    check_native_parity({0x83, 0x58, 0x04, 0x7F},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.flags.set_cf(true);
        });
    auto stored = m_memory->read_u32(data_address + 4);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0x7FFFFF80u);
}

TEST_F(JitParity64Test, NativeSbbR32Rm32MemorySourceUsesFullEa) {
    // SBB ECX,dword ptr [RAX+4] — 1B 48 04. This is the opposite ModRM
    // direction and covers the second hot SBB opcode.
    constexpr GuestAddr data_address = 0x140008000ull;
    ASSERT_TRUE(m_memory->map_region(data_address, 0x1000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u32(data_address + 4, 0x00000020u));
    check_native_parity({0x1B, 0x48, 0x04},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.gpr.set64(RCX, 0xCCCCDDDD00000010ull);
            cpu.flags.set_cf(false);
        });
}

// ============================================================================
// MOVSXD (64-bit sign-extend 32→64)
// ============================================================================

TEST_F(JitParity64Test, Movsxd) {
    // MOVSXD RAX, EBX — 48 63 C3
    check_parity({0x48, 0x63, 0xC3},
        [](CpuState& cpu) { cpu.gpr.set32(RBX, 0x80000000); },
        false);
}

TEST_F(JitParity64Test, NativeMovsxR64Rm8Register) {
    // MOVSX RAX,CL — 48 0F BE C1.
    check_native_parity({0x48, 0x0F, 0xBE, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x0123456789ABCDEFull);
            cpu.gpr.set64(RCX, 0x1111111111111180ull);
            cpu.flags.rflags = 0x8D5u;
        });
}

TEST_F(JitParity64Test, NativeMovsxR64Rm8MemoryUsesFullEa) {
    constexpr GuestAddr data_address = 0x14000C000ull;
    ASSERT_TRUE(m_memory->map_region(data_address, 0x1000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u8(data_address + 5, 0x81u).is_ok());
    // MOVSX R9,byte ptr [RAX+5] — 4C 0F BE 48 05.
    check_native_parity({0x4C, 0x0F, 0xBE, 0x48, 0x05},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.gpr.set64(static_cast<u8>(RegIndex::R9),
                          0x7777777777777777ull);
            cpu.flags.rflags = 0x8D5u;
        });
}

TEST_F(JitParity64Test, NativeBsrR64NonzeroAndZero) {
    // BSR R9,R10 — 4D 0F BD CA.
    check_native_parity({0x4D, 0x0F, 0xBD, 0xCA},
        [](CpuState& cpu) {
            cpu.gpr.set64(static_cast<u8>(RegIndex::R9),
                          0x7777777777777777ull);
            cpu.gpr.set64(static_cast<u8>(RegIndex::R10), 1ull << 47);
            cpu.flags.rflags = 0x895u;
        });
    check_native_parity({0x4D, 0x0F, 0xBD, 0xCA},
        [](CpuState& cpu) {
            cpu.gpr.set64(static_cast<u8>(RegIndex::R9),
                          0x7777777777777777ull);
            cpu.gpr.set64(static_cast<u8>(RegIndex::R10), 0);
            cpu.flags.rflags = 0x895u;
        });
}

TEST_F(JitParity64Test, NativeBsrR64MemoryUsesFullEa) {
    constexpr GuestAddr data_address = 0x14000F000ull;
    ASSERT_TRUE(m_memory->map_region(data_address, 0x1000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u64(data_address + 8, 1ull << 63).is_ok());
    // BSR R9,[RAX+8] — 4C 0F BD 48 08.
    check_native_parity({0x4C, 0x0F, 0xBD, 0x48, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.gpr.set64(static_cast<u8>(RegIndex::R9),
                          0x7777777777777777ull);
            cpu.flags.rflags = 0x895u;
        });
}

TEST_F(JitParity64Test, NativeBsfR64NonzeroAndZero) {
    // BSF R9,R10 — 4D 0F BC CA.
    check_native_parity({0x4D, 0x0F, 0xBC, 0xCA},
        [](CpuState& cpu) {
            cpu.gpr.set64(static_cast<u8>(RegIndex::R9),
                          0x7777777777777777ull);
            cpu.gpr.set64(static_cast<u8>(RegIndex::R10), 1ull << 47);
            cpu.flags.rflags = 0x895u;
        });
    check_native_parity({0x4D, 0x0F, 0xBC, 0xCA},
        [](CpuState& cpu) {
            cpu.gpr.set64(static_cast<u8>(RegIndex::R9),
                          0x7777777777777777ull);
            cpu.gpr.set64(static_cast<u8>(RegIndex::R10), 0);
            cpu.flags.rflags = 0x895u;
        });
}

TEST_F(JitParity64Test, NativeBsfR64MemoryUsesFullEa) {
    constexpr GuestAddr data_address = 0x140010000ull;
    ASSERT_TRUE(m_memory->map_region(data_address, 0x1000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u64(data_address + 8, 1ull << 12).is_ok());
    // BSF R9,[RAX+8] — 4C 0F BC 48 08.
    check_native_parity({0x4C, 0x0F, 0xBC, 0x48, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.gpr.set64(static_cast<u8>(RegIndex::R9),
                          0x7777777777777777ull);
            cpu.flags.rflags = 0x895u;
        });
}

// ============================================================================
// 64-bit PUSH / POP (8-byte operations)
// ============================================================================

TEST_F(JitParity64Test, PushPop64) {
    // PUSH RAX; POP RBX — 50 5B
    check_parity({0x50, 0x5B},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xDEADBEEFCAFEBABEull);
            cpu.gpr.set64(RBX, 0);
        },
        false);
}

TEST_F(JitParity64Test, NativeOrAlImm8PreservesUpperRaxAndClearsCarry) {
    // OR AL,0x81 — 0C 81.
    check_native_parity({0x0C, 0x81},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x1122334455667702ull);
            cpu.flags.rflags = 0x8D5u;
        });
}

TEST_F(JitParity64Test, NativeOrRm8R8MemoryRmw) {
    ASSERT_TRUE(m_memory->write_u8(0x3003, 0x80u).is_ok());
    // OR byte ptr [RAX+3],CL — 08 48 03.
    check_native_parity({0x08, 0x48, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.gpr.set64(RCX, 0xAAAAAAAAAAAAAA05ull);
            cpu.flags.rflags = 0x8D5u;
        });
}

TEST_F(JitParity64Test, NativeOrR8Rm8MemoryUsesFullEa) {
    constexpr GuestAddr data_address = 0x14000E000ull;
    ASSERT_TRUE(m_memory->map_region(data_address, 0x1000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u8(data_address + 3, 0x81u).is_ok());
    // OR CL,byte ptr [RAX+3] — 0A 48 03.
    check_native_parity({0x0A, 0x48, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.gpr.set64(RCX, 0xAAAAAAAAAAAAAA04ull);
            cpu.flags.rflags = 0x8D5u;
        });
}

TEST_F(JitParity64Test, NativeIncDecBytePreserveCarryAndPartialRegister) {
    // INC AL — FE C0. 0x7F -> 0x80 sets OF while preserving set CF and all
    // non-AL bits of RAX.
    check_native_parity({0xFE, 0xC0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x112233445566777Full);
            cpu.flags.rflags = 0x2u;
            cpu.flags.set_cf(true);
        });

    // DEC AH — FE CC. 0x80 -> 0x7F sets OF while preserving clear CF and
    // every byte except AH.
    check_native_parity({0xFE, 0xCC},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x11223344556680AAull);
            cpu.flags.rflags = 0x2u;
            cpu.flags.set_cf(false);
        });
}

TEST_F(JitParity64Test, NativeIncByteMemoryRmw) {
    ASSERT_TRUE(m_memory->write_u8(0x3003, 0xFFu).is_ok());
    // INC byte ptr [RAX+3] — FE 40 03.
    check_native_parity({0xFE, 0x40, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.flags.rflags = 0x2u;
            cpu.flags.set_cf(true);
        });
}
