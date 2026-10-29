// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): MOV, ALU,
// CMP/TEST, shifts and rotates.

#include "jit_parity_64_fixture.hpp"

TEST_F(JitParity64Test, PinnedCmp32ZeroUsesOneNativeCompare) {
    const u8 code[] = {
        0x83, 0xF8, 0x00,       // cmp eax,0
        0x0F, 0x94, 0xC3,       // setz bl (forces lazy flags to remain valid)
    };

    check_parity(code, sizeof(code), [](CpuState& cpu) {
        cpu.gpr.set32(RAX, 0);
    });
    check_parity(code, sizeof(code), [](CpuState& cpu) {
        cpu.gpr.set32(RAX, 7);
    });

    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    // The parity checks above are the correctness claim. What follows asserts
    // the EMISSION SHAPE, and it is now asserted on BOTH tiers: Tier 1 binds a
    // zero constant to the zero register instead of materializing it, and
    // recognises that a subtraction of zero whose result only feeds a flag
    // definition needs no subtraction at all -- the result is the first
    // operand, so only NZCV has to be published.
    JitFaultMetadata metadata{};
    ASSERT_TRUE(jit_fault_metadata_for_pc(
        reinterpret_cast<uintptr_t>(compiled->code) +
            compiled->link_code_offset,
        metadata));
    const u8 rax_pin = metadata.pins.host_reg[RAX];
    ASSERT_NE(rax_pin, 0xFFu);
    const auto* words = static_cast<const u32*>(compiled->code);
    const usize word_count = compiled->code_size / sizeof(u32);
    const u32 direct_cmp = arm64::subs_w(31, rax_pin, 31);
    EXPECT_NE(std::find(words, words + word_count, direct_cmp),
              words + word_count)
        << "CMP EAX,0 did not compare the pin directly against WZR";
    EXPECT_EQ(std::find(words, words + word_count,
                        arm64::movz_w(1, 0)),
              words + word_count)
        << "CMP EAX,0 regressed to materializing a zero immediate";
}

TEST_F(JitParity64Test, MemoryMovLoadsDirectlyIntoPinnedDestination) {
    constexpr GuestAddr data = 0x3000;
    ASSERT_TRUE(m_memory->write_u64(data, 0x1122334455667788ull));

    const u8 mov32[] = {0x8B, 0x03};       // mov eax,[rbx]
    check_parity(mov32, sizeof(mov32), [](CpuState& cpu) {
        cpu.gpr.set64(RBX, data);
    });
    BasicBlock block32 = build_block(mov32, sizeof(mov32));
    CompiledBlock* compiled32 = m_jit->compile(block32);
    ASSERT_NE(compiled32, nullptr);
    const auto* words32 = static_cast<const u32*>(compiled32->code);
    const usize count32 = compiled32->code_size / sizeof(u32);
    EXPECT_TRUE(std::find(words32, words32 + count32,
                          arm64::ldapr_w(23, 17)) != words32 + count32 ||
                std::find(words32, words32 + count32,
                          arm64::ldar_w(23, 17)) != words32 + count32)
        << "MOV EAX,[mem] does not load directly into pinned W23";

    const u8 mov64[] = {0x48, 0x8B, 0x03}; // mov rax,[rbx]
    check_parity(mov64, sizeof(mov64), [](CpuState& cpu) {
        cpu.gpr.set64(RBX, data);
    });
    BasicBlock block64 = build_block(mov64, sizeof(mov64));
    CompiledBlock* compiled64 = m_jit->compile(block64);
    ASSERT_NE(compiled64, nullptr);
    const auto* words64 = static_cast<const u32*>(compiled64->code);
    const usize count64 = compiled64->code_size / sizeof(u32);
    EXPECT_TRUE(std::find(words64, words64 + count64,
                          arm64::ldapr_x(23, 17)) != words64 + count64 ||
                std::find(words64, words64 + count64,
                          arm64::ldar_x(23, 17)) != words64 + count64)
        << "MOV RAX,[mem] does not load directly into pinned X23";
}

// ============================================================================
// 64-bit MOV
// ============================================================================

TEST_F(JitParity64Test, MovR64Imm64) {
    // MOV RAX, 0x123456789ABCDEF0 — 48 B8 + 8 bytes
    u8 code[] = {0x48, 0xB8, 0xF0, 0xDE, 0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12};
    check_parity(code, sizeof(code), {}, false);
}

TEST_F(JitParity64Test, MovR64R64) {
    // MOV RAX, RBX — 48 8B C3
    check_parity({0x48, 0x8B, 0xC3},
        [](CpuState& cpu) { cpu.gpr.set64(RBX, 0xDEADBEEFCAFEBABEull); },
        false);
}

TEST_F(JitParity64Test, MovR32Imm32ClearsUpper) {
    // MOV EAX, 0x12345678 — B8 78563412
    // In 64-bit mode, 32-bit MOV clears upper 32 bits
    check_parity({0xB8, 0x78, 0x56, 0x34, 0x12},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0xFFFFFFFF00000000ull); },
        false);
}

TEST_F(JitParity64Test, NativeByteMovPreservesUpperBitsAndRexSelection) {
    // MOV AH,BL; MOV SPL,CL; MOV R8B,DIL; MOV R9B,0xDD. The first instruction
    // has no REX and selects a legacy high byte; the latter three use REX byte
    // registers. None may change flags or the other 56 destination bits.
    check_native_parity({
            0x88, 0xDC,
            0x40, 0x88, 0xCC,
            0x41, 0x88, 0xF8,
            0x41, 0xB1, 0xDD,
        },
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x1122334455667788ull);
            cpu.gpr.set64(RBX, 0x88776655443322AAull);
            cpu.gpr.set64(RCX, 0x01020304050607BBull);
            cpu.gpr.set64(RSP, 0xFFEEDDCCBBAA9988ull);
            cpu.gpr.set64(RDI, 0x10203040506070CCull);
            cpu.gpr.set64(R8,  0xA1A2A3A4A5A6A7A8ull);
            cpu.gpr.set64(static_cast<u8>(RegIndex::R9),
                          0xB1B2B3B4B5B6B7B8ull);
        });
}

TEST_F(JitParity64Test, NativeByteMovMemoryUsesFullEaAndExactWidth) {
    // MOV [RAX+3],BL; MOV CL,[RAX+3]; MOV byte ptr [RAX+4],0x5A.
    check_native_parity({
            0x88, 0x58, 0x03,
            0x8A, 0x48, 0x03,
            0xC6, 0x40, 0x04, 0x5A,
        },
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x2040);
            cpu.gpr.set64(RBX, 0x0123456789ABCDEFull);
            cpu.gpr.set64(RCX, 0xFFEEDDCCBBAA9988ull);
        });
    u8 out[4] = {};
    ASSERT_TRUE(m_memory->read_bytes(0x2042, out, sizeof(out)).is_ok());
    EXPECT_EQ(out[0], 0);
    EXPECT_EQ(out[1], 0xEF);
    EXPECT_EQ(out[2], 0x5A);
    EXPECT_EQ(out[3], 0);
}

TEST_F(JitParity64Test, NativeXorR8Rm8MemoryUsesFullEaAndPreservesGpr) {
    // XOR R9B, byte ptr [RAX+2] — 44 32 48 02. The source is above 4 GB,
    // and only R9's low byte may change despite the extended-byte encoding.
    constexpr GuestAddr data_address = 0x140006000ull;
    ASSERT_TRUE(m_memory->map_region(data_address, 0x1000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u8(data_address + 2, 0xFF));
    check_native_parity({0x44, 0x32, 0x48, 0x02},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.gpr.set64(static_cast<u8>(RegIndex::R9),
                          0xAAAABBBBCCCCDD80ull);
            cpu.flags.rflags = 0x8D5u;
        });
}

TEST_F(JitParity64Test, NativeXorHighByteRegisterPreservesOtherBits) {
    // XOR AH,CL — 32 E1. Without REX, register code 4 selects AH rather than
    // SPL; the rest of RAX and all of RCX must remain unchanged.
    check_native_parity({0x32, 0xE1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x111122223333A544ull);
            cpu.gpr.set64(RCX, 0xAAAABBBBCCCCDD5Aull);
            cpu.flags.rflags = 0x8D5u;
        });
}

TEST_F(JitParity64Test, NativeSubAlImm8PreservesUpperRax) {
    // SUB AL,1 — 2C 01. Only AL changes while all arithmetic flags are
    // derived from the wrapped byte result.
    check_native_parity({0x2C, 0x01},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xAAAABBBBCCCCDD00ull);
            cpu.flags.rflags = 0x8D5u;
        });
}

TEST_F(JitParity64Test, NativeSubRm8Imm8MemoryRmw) {
    // SUB byte ptr [RAX+2],0x7F — 80 68 02 7F.
    ASSERT_TRUE(m_memory->write_u8(0x2042, 0x80));
    check_native_parity({0x80, 0x68, 0x02, 0x7F},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x2040); });
    auto stored = m_memory->read_u8(0x2042);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0x01);
}

TEST_F(JitParity64Test, NativeSubR8Rm8MemoryUsesFullEa) {
    // SUB R9B,byte ptr [RAX+3] — 44 2A 48 03. The extended byte destination
    // retains its other 56 bits and the source address remains 64-bit.
    constexpr GuestAddr data_address = 0x140009000ull;
    ASSERT_TRUE(m_memory->map_region(data_address, 0x1000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u8(data_address + 3, 0x22));
    check_native_parity({0x44, 0x2A, 0x48, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.gpr.set64(static_cast<u8>(RegIndex::R9),
                          0x1111222233334480ull);
        });
}

TEST_F(JitParity64Test, NativeSubHighByteRegisterPreservesOtherBits) {
    // SUB AH,CL — 28 CC. No REX means r/m code 4 selects AH.
    check_native_parity({0x28, 0xCC},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x1111222233338044ull);
            cpu.gpr.set64(RCX, 0xAAAABBBBCCCCDD01ull);
        });
}

TEST_F(JitParity64Test, NativeAndAlImm8PreservesUpperRaxAndClearsCarry) {
    // AND AL,0x0F — 24 0F. The logical result replaces AL only and clears
    // CF/OF while deriving SF/ZF/PF from the byte result.
    check_native_parity({0x24, 0x0F},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xAAAABBBBCCCCDDF0ull);
            cpu.flags.set_cf(true);
            cpu.flags.set_of(true);
        });
}

TEST_F(JitParity64Test, NativeAndRm8Imm8MemoryRmw) {
    // AND byte ptr [RAX+2],0x0F — 80 60 02 0F.
    ASSERT_TRUE(m_memory->write_u8(0x2042, 0xA5));
    check_native_parity({0x80, 0x60, 0x02, 0x0F},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x2040); });
    auto stored = m_memory->read_u8(0x2042);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0x05);
}

TEST_F(JitParity64Test, NativeAndByteDirectionalForms) {
    // AND R9B,[RAX+3]; AND AH,CL. Together these cover a full-width memory
    // source, REX extended byte, and the legacy high-byte destination.
    constexpr GuestAddr data_address = 0x14000A000ull;
    ASSERT_TRUE(m_memory->map_region(data_address, 0x1000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u8(data_address + 3, 0x0F));
    check_native_parity({
            0x44, 0x22, 0x48, 0x03,
            0x20, 0xCC,
        },
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.gpr.set64(static_cast<u8>(RegIndex::R9),
                          0x11112222333344F5ull);
            cpu.gpr.set64(RCX, 0xAAAABBBBCCCCDD55ull);
        });
}

// ============================================================================
// 64-bit ADD / SUB
// ============================================================================

TEST_F(JitParity64Test, AddR64R64) {
    // ADD RAX, RBX — 48 01 D8
    check_parity({0x48, 0x01, 0xD8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x100000000ull);
            cpu.gpr.set64(RBX, 0x200000000ull);
        });
}

TEST_F(JitParity64Test, SubR64R64) {
    // SUB RAX, RBX — 48 29 D8
    check_parity({0x48, 0x29, 0xD8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x300000000ull);
            cpu.gpr.set64(RBX, 0x100000000ull);
        });
}

TEST_F(JitParity64Test, NativeSubR64FromMemory) {
    ASSERT_TRUE(m_memory->write_u64(0x3000, 0x100000000ull));
    // SUB RAX, qword ptr [RBX] — 48 2B 03. This is the opposite ModRM
    // direction from SubR64R64 and exercises the 64-bit read helper.
    check_native_parity({0x48, 0x2B, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x300000000ull);
            cpu.gpr.set64(RBX, 0x3000);
        });
}

TEST_F(JitParity64Test, NativeSubR64FromMemoryWithDestinationAsBase) {
    ASSERT_TRUE(m_memory->write_u64(0x3000, 0x100ull));
    // SUB RAX, qword ptr [RAX] must form the effective address from the
    // original RAX before writing the subtraction result back to RAX.
    check_native_parity({0x48, 0x2B, 0x00},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x3000); });
}

TEST_F(JitParity64Test, NativeIncR64PreservesSetCarry) {
    // INC RAX. The result has bit 32 clear, deliberately disagreeing with CF.
    check_native_parity({0x48, 0xFF, 0xC0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0);
            cpu.flags.set_cf(true);
        });
}

TEST_F(JitParity64Test, NativeDecR64PreservesClearCarry) {
    // DEC RAX. The result has bit 32 set, deliberately disagreeing with CF.
    check_native_parity({0x48, 0xFF, 0xC8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x100000001ull);
            cpu.flags.set_cf(false);
        });
}

TEST_F(JitParity64Test, AddR64Overflow) {
    check_parity({0x48, 0x01, 0xD8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RBX, 1);
        });
}

// ============================================================================
// 64-bit logical ops
// ============================================================================

TEST_F(JitParity64Test, XorR64Self) {
    // XOR RAX, RAX — 48 31 C0
    check_parity({0x48, 0x31, 0xC0},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0xDEADBEEFCAFEBABEull); });
}

TEST_F(JitParity64Test, AndR64R64) {
    // AND RAX, RBX — 48 21 D8
    check_parity({0x48, 0x21, 0xD8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFF00FF00FF00FF00ull);
            cpu.gpr.set64(RBX, 0x0FF00FF00FF00FF0ull);
        });
}

TEST_F(JitParity64Test, OrR64R64) {
    // OR RAX, RBX — 48 09 D8
    check_parity({0x48, 0x09, 0xD8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFF00FF0000000000ull);
            cpu.gpr.set64(RBX, 0x00000000FF00FF00ull);
        });
}

// ============================================================================
// 64-bit CMP / TEST
// ============================================================================

TEST_F(JitParity64Test, CmpR64R64Equal) {
    // CMP RAX, RBX — 48 39 D8
    check_parity({0x48, 0x39, 0xD8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x100000000ull);
            cpu.gpr.set64(RBX, 0x100000000ull);
        });
}

TEST_F(JitParity64Test, CmpR64R64Less) {
    check_parity({0x48, 0x39, 0xD8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x1ull);
            cpu.gpr.set64(RBX, 0x100000000ull);
        });
}

TEST_F(JitParity64Test, TestR64R64) {
    // TEST RAX, RAX — 48 85 C0
    check_parity({0x48, 0x85, 0xC0},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x8000000000000000ull); });
}

// ============================================================================
// 64-bit shifts
// ============================================================================

TEST_F(JitParity64Test, ShlR64Imm) {
    // SHL RAX, 32 — 48 C1 E0 20
    check_parity({0x48, 0xC1, 0xE0, 0x20},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 1); });
}

TEST_F(JitParity64Test, ShrR64Imm) {
    // SHR RAX, 32 — 48 C1 E8 20
    check_parity({0x48, 0xC1, 0xE8, 0x20},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x100000000ull); });
}

// ============================================================================
// ROR r/m32 (32-bit-operand rotate inside a 64-bit block)
//
// ROR is a partial-flag op: only CF (and OF for count==1) change; SF/ZF/PF/AF
// are preserved. The native x64 path must (a) zero-extend the 32-bit result
// into the full 64-bit GPR and (b) not clobber the preserved flags — even when
// a preceding instruction left flags pending (the mixed-block hazard that got
// an earlier native attempt rejected).
// ============================================================================

TEST_F(JitParity64Test, RorR32Imm) {
    // ROR ECX, 3 — C1 C9 03.  Verifies CF + upper-32 zero-extension of RCX.
    check_parity({0xC1, 0xC9, 0x03},
        [](CpuState& cpu) { cpu.gpr.set64(RCX, 0xFFFFFFFF12345671ull); });
}

TEST_F(JitParity64Test, RorR32ImmMsbSet) {
    // ROR EAX, 5 with a value whose rotated MSB is 1 (CF must be set).
    check_parity({0xC1, 0xC8, 0x05},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x0000001Full); });
}

TEST_F(JitParity64Test, RorR32By1ImmForm) {
    // ROR EBX, 1 via the imm form — C1 CB 01.  OF is defined for count==1.
    check_parity({0xC1, 0xCB, 0x01},
        [](CpuState& cpu) { cpu.gpr.set64(RBX, 0x80000001ull); });
}

TEST_F(JitParity64Test, RorR32By1D1Form) {
    // ROR EDX, 1 via the D1 /1 short form — D1 CA.
    check_parity({0xD1, 0xCA},
        [](CpuState& cpu) { cpu.gpr.set64(RDX, 0x00000003ull); });
}

TEST_F(JitParity64Test, RorR32ByZeroPreservesFlags) {
    // ROR ESI, 0 — C1 CE 00.  Count 0 leaves dst and ALL flags untouched;
    // seed a distinctive flag state and confirm both engines preserve it.
    check_parity({0xC1, 0xCE, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RSI, 0xAABBCCDDull);
            cpu.flags.set_cf(true); cpu.flags.set_zf(true);
            cpu.flags.set_sf(true); cpu.flags.set_of(true);
        });
}

TEST_F(JitParity64Test, RorR32MixedBlockPreservesZf) {
    // Regression for the mixed-block flag hazard: SUB ECX,ECX sets ZF=1 (and
    // clears CF/SF/OF), then ROR EAX,3 must leave ZF/SF/PF intact while only
    // updating CF.  If ROR emits a lazy op that overwrites the pending SUB
    // flags, the final ZF diverges from the interpreter.
    //   SUB ECX, ECX — 29 C9 ; ROR EAX, 3 — C1 C8 03
    check_parity({0x29, 0xC9, 0xC1, 0xC8, 0x03},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x00000007ull); });
}

TEST_F(JitParity64Test, RorR32AfterCmpPreservesFlags) {
    // CMP EAX,EBX (equal -> ZF=1) ; ROR EDX,7 ; the compare's flags must
    // survive the rotate for a following conditional to read them correctly.
    //   CMP EAX, EBX — 39 D8 ; ROR EDX, 7 — C1 CA 07
    check_parity({0x39, 0xD8, 0xC1, 0xCA, 0x07},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x42);
            cpu.gpr.set64(RBX, 0x42);
            cpu.gpr.set64(RDX, 0x91117777ull);
        });
}

// ---------------------------------------------------------------------------
// ROL r/m32 (32-bit-operand rotate inside a 64-bit block)
//
// Same partial-flag contract as ROR: CF (and OF for count==1) only. Native
// path must zero-extend the 32-bit result and preserve SF/ZF/PF across a
// pending FlagOp from a preceding arithmetic op.
// ---------------------------------------------------------------------------

TEST_F(JitParity64Test, RolR32Imm) {
    // ROL ECX, 3 — C1 C1 03.  Verifies CF + upper-32 zero-extension of RCX.
    check_parity({0xC1, 0xC1, 0x03},
        [](CpuState& cpu) { cpu.gpr.set64(RCX, 0xFFFFFFFF12345671ull); });
}

TEST_F(JitParity64Test, RolR32ImmLsbIntoCf) {
    // ROL EAX, 5; MSB of input becomes bit4 of result; CF = LSB after rotate.
    check_parity({0xC1, 0xC0, 0x05},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x80000001ull); });
}

TEST_F(JitParity64Test, RolR32By1ImmForm) {
    // ROL EBX, 1 via the imm form — C1 C3 01.  OF is defined for count==1.
    check_parity({0xC1, 0xC3, 0x01},
        [](CpuState& cpu) { cpu.gpr.set64(RBX, 0x80000001ull); });
}

TEST_F(JitParity64Test, RolR32By1D1Form) {
    // ROL EDX, 1 via the D1 /0 short form — D1 C2.
    check_parity({0xD1, 0xC2},
        [](CpuState& cpu) { cpu.gpr.set64(RDX, 0x40000000ull); });
}

TEST_F(JitParity64Test, RolR32ByZeroPreservesFlags) {
    // ROL ESI, 0 — C1 C6 00.  Count 0 leaves dst and ALL flags untouched.
    check_parity({0xC1, 0xC6, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RSI, 0xAABBCCDDull);
            cpu.flags.set_cf(true); cpu.flags.set_zf(true);
            cpu.flags.set_sf(true); cpu.flags.set_of(true);
        });
}

TEST_F(JitParity64Test, RolR32MixedBlockPreservesZf) {
    // SUB ECX,ECX sets ZF=1; ROL EAX,3 must leave ZF/SF/PF intact.
    //   SUB ECX, ECX — 29 C9 ; ROL EAX, 3 — C1 C0 03
    check_parity({0x29, 0xC9, 0xC1, 0xC0, 0x03},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x00000007ull); });
}

TEST_F(JitParity64Test, RolR32AfterCmpPreservesFlags) {
    // CMP EAX,EBX (equal -> ZF=1) ; ROL EDX,7 ; compare flags must survive.
    //   CMP EAX, EBX — 39 D8 ; ROL EDX, 7 — C1 C2 07
    check_parity({0x39, 0xD8, 0xC1, 0xC2, 0x07},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x42);
            cpu.gpr.set64(RBX, 0x42);
            cpu.gpr.set64(RDX, 0x91117777ull);
        });
}

TEST_F(JitParity64Test, NativeTestRm16ImmediatePreservesRegister) {
    // TEST AX, 0x8000 via F7 /0. The AND result affects flags only; all 64
    // bits of RAX must remain unchanged despite the 16-bit operand size.
    check_native_parity({0x66, 0xF7, 0xC0, 0x00, 0x80},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xAAAABBBBCCCC8000ull);
            cpu.flags.set_cf(true);
            cpu.flags.set_of(true);
        });
}

TEST_F(JitParity64Test, NativeTestRm16ImmediateMemoryUsesFullEa) {
    // TEST word ptr [RAX+2], 0x8000. A PE32+-range address proves that the
    // native stencil does not truncate the long-mode effective address.
    constexpr GuestAddr data_address = 0x140005000ull;
    ASSERT_TRUE(m_memory->map_region(data_address, 0x1000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u16(data_address + 2, 0x7FFF));
    check_native_parity({0x66, 0xF7, 0x40, 0x02, 0x00, 0x80},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.flags.set_cf(true);
            cpu.flags.set_of(true);
        });
    auto value = m_memory->read_u16(data_address + 2);
    ASSERT_TRUE(value.is_ok());
    EXPECT_EQ(value.value(), 0x7FFF);
}

TEST_F(JitParity64Test, NativeTestRm16R16PreservesOperands) {
    // TEST AX, CX — 66 85 C8. Exercise the register-source encoding and
    // verify that neither partial-register operand is written back.
    check_native_parity({0x66, 0x85, 0xC8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x1111222233338001ull);
            cpu.gpr.set64(RCX, 0xAAAABBBBCCCC0001ull);
            cpu.flags.set_cf(true);
            cpu.flags.set_of(true);
        });
}
