// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): ADX, MULX,
// 32-bit SHLD/SHRD and MUL/DIV.

#include "jit_parity_64_fixture.hpp"

// ---------------------------------------------------------------------------
// ADX against a pending lazy FlagOp.
//
// Rosetta #UDs ADCX/ADOX, so the oracle cannot check these at all and the
// interpreter is the reference (itself validated on real silicon via
// Windows -- see tests/cpu/test_adx_lazy_flags.cpp). What matters here is
// that the JIT lowering writes only its own overlay and leaves the pending
// FlagOp owning every flag ADX preserves.
// ---------------------------------------------------------------------------

TEST_F(JitParity64Test, AdcxAfterAddConsumesLazyCarry) {
    // ADD eax, ecx ; ADCX edx, ebx  (66 0F 38 F6 D3)
    check_parity({0x01, 0xC8, 0x66, 0x0F, 0x38, 0xF6, 0xD3},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFull);  // ADD carries out
            cpu.gpr.set64(RCX, 1);
            cpu.gpr.set64(RDX, 0x10);
            cpu.gpr.set64(RBX, 0x20);
        });
}

TEST_F(JitParity64Test, AdcxAfterNonCarryingAdd) {
    check_parity({0x01, 0xC8, 0x66, 0x0F, 0x38, 0xF6, 0xD3},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 1);
            cpu.gpr.set64(RCX, 1);
            cpu.gpr.set64(RDX, 0x10);
            cpu.gpr.set64(RBX, 0x20);
        });
}

// ADCX's own result is zero here; ZF must still come from the ADD, not from
// ADCX (silicon: ADCX does not write ZF).
TEST_F(JitParity64Test, AdcxZeroResultDoesNotWriteZf) {
    check_parity({0x01, 0xC8, 0x66, 0x0F, 0x38, 0xF6, 0xD3},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 1);
            cpu.gpr.set64(RCX, 1);
            cpu.gpr.set64(RDX, 0xFFFFFFFFull);
            cpu.gpr.set64(RBX, 1);
        });
}

TEST_F(JitParity64Test, Adcx64AfterAdd) {
    // ADD rax, rcx ; ADCX rdx, rbx  (66 REX.W 0F 38 F6 D3)
    check_parity({0x48, 0x01, 0xC8, 0x66, 0x48, 0x0F, 0x38, 0xF6, 0xD3},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RCX, 1);
            cpu.gpr.set64(RDX, 0x10);
            cpu.gpr.set64(RBX, 0x20);
        });
}

// --- ADX carry chains (the BoringSSL bn_mulx4x_mont shape) ---------------
// The single-instruction cases above never exercise the pattern big-integer
// code actually emits: a *chain* where one ADCX consumes the CF overlay the
// previous ADCX wrote, and ADCX/ADOX interleaved so their two independent
// carry chains must not disturb each other. RSA-4096 verification is built
// entirely out of this shape.

// ADCX consuming the carry a previous ADCX produced, three links deep.
TEST_F(JitParity64Test, AdcxChainConsumesPreviousAdcxCarry) {
    // ADD rax,rcx ; ADCX rdx,rbx ; ADCX rsi,rdi ; ADCX r8,r9
    check_native_parity({0x48, 0x01, 0xC8,
                  0x66, 0x48, 0x0F, 0x38, 0xF6, 0xD3,
                  0x66, 0x48, 0x0F, 0x38, 0xF6, 0xF7,
                  0x66, 0x4D, 0x0F, 0x38, 0xF6, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RCX, 1);
            cpu.gpr.set64(RDX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RBX, 0);
            cpu.gpr.set64(RSI, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RDI, 0);
            cpu.gpr.set64(R8, 0x1234);
            cpu.gpr.set64(R9, 0x5678);
        });
}

// ADOX consuming the overflow a previous ADOX produced.
TEST_F(JitParity64Test, AdoxChainConsumesPreviousAdoxOverflow) {
    // ADD rax,rcx ; ADOX rdx,rbx ; ADOX rsi,rdi ; ADOX r8,r9
    check_native_parity({0x48, 0x01, 0xC8,
                  0xF3, 0x48, 0x0F, 0x38, 0xF6, 0xD3,
                  0xF3, 0x48, 0x0F, 0x38, 0xF6, 0xF7,
                  0xF3, 0x4D, 0x0F, 0x38, 0xF6, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RCX, 1);
            cpu.gpr.set64(RDX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RBX, 0);
            cpu.gpr.set64(RSI, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RDI, 0);
            cpu.gpr.set64(R8, 0x1234);
            cpu.gpr.set64(R9, 0x5678);
        });
}

// The real Montgomery shape: two independent carry chains interleaved. Each
// ADCX must ignore the OF ADOX just wrote and vice versa.
TEST_F(JitParity64Test, AdcxAdoxInterleavedDualCarryChains) {
    // ADD rax,rcx ; ADCX rdx,rbx ; ADOX rsi,rdi ; ADCX r8,r9 ; ADOX r10,r11
    check_native_parity({0x48, 0x01, 0xC8,
                  0x66, 0x48, 0x0F, 0x38, 0xF6, 0xD3,
                  0xF3, 0x48, 0x0F, 0x38, 0xF6, 0xF7,
                  0x66, 0x4D, 0x0F, 0x38, 0xF6, 0xC1,
                  0xF3, 0x4D, 0x0F, 0x38, 0xF6, 0xD3},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RCX, 1);
            cpu.gpr.set64(RDX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RBX, 0);
            cpu.gpr.set64(RSI, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RDI, 0);
            cpu.gpr.set64(R8, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(R9, 0);
            cpu.gpr.set64(R10, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(R11, 0);
        });
}

// MULX feeding an ADCX/ADOX pair, which is literally one limb step of
// bn_mulx4x_mont.
TEST_F(JitParity64Test, MulxFeedingAdcxAdoxLimbStep) {
    // MULX r8, r9, rbx  (VEX.NDS.LZ.F2.0F38.W1 F6 /r) ; ADCX r8,r10 ; ADOX r9,r11
    check_native_parity({0xC4, 0x42, 0xB3, 0xF6, 0xC3,
                  0x66, 0x4D, 0x0F, 0x38, 0xF6, 0xC2,
                  0xF3, 0x4D, 0x0F, 0x38, 0xF6, 0xCB},
        [](CpuState& cpu) {
            cpu.gpr.set64(RDX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RBX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(R10, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(R11, 0xFFFFFFFFFFFFFFFFull);
        });
}

// Memory-operand ADX: the form bn_mulx4x_mont actually emits
// (`adcx 0*8(%rsi),%r8`). The lowering must get the carry-in into the
// callee-saved REG_CALL_TMP *before* the operand load, because a non-linear
// guest load can call a helper that clobbers the scratch registers.
TEST_F(JitParity64Test, AdcxMemoryOperandChainKeepsCarry) {
    ASSERT_TRUE(m_memory->write_u64(0x2000, 0xFFFFFFFFFFFFFFFFull).is_ok());
    ASSERT_TRUE(m_memory->write_u64(0x2008, 0xFFFFFFFFFFFFFFFFull).is_ok());
    // ADD rax,rcx ; ADCX r8,[rsi] ; ADCX r9,[rsi+8]
    check_native_parity({0x48, 0x01, 0xC8,
                         0x66, 0x4C, 0x0F, 0x38, 0xF6, 0x06,
                         0x66, 0x4C, 0x0F, 0x38, 0xF6, 0x4E, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RCX, 1);
            cpu.gpr.set64(RSI, 0x2000);
            cpu.gpr.set64(R8, 1);
            cpu.gpr.set64(R9, 1);
        });
}

TEST_F(JitParity64Test, AdoxMemoryOperandChainKeepsOverflow) {
    ASSERT_TRUE(m_memory->write_u64(0x2000, 0xFFFFFFFFFFFFFFFFull).is_ok());
    ASSERT_TRUE(m_memory->write_u64(0x2008, 0xFFFFFFFFFFFFFFFFull).is_ok());
    // ADD rax,rcx ; ADOX r8,[rsi] ; ADOX r9,[rsi+8]
    check_native_parity({0x48, 0x01, 0xC8,
                         0xF3, 0x4C, 0x0F, 0x38, 0xF6, 0x06,
                         0xF3, 0x4C, 0x0F, 0x38, 0xF6, 0x4E, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RCX, 1);
            cpu.gpr.set64(RSI, 0x2000);
            cpu.gpr.set64(R8, 1);
            cpu.gpr.set64(R9, 1);
        });
}

// Interleaved dual carry chains with memory operands — the full limb step.
TEST_F(JitParity64Test, AdcxAdoxMemoryInterleavedChains) {
    ASSERT_TRUE(m_memory->write_u64(0x2000, 0xFFFFFFFFFFFFFFFFull).is_ok());
    ASSERT_TRUE(m_memory->write_u64(0x2008, 0xFFFFFFFFFFFFFFFFull).is_ok());
    // ADD rax,rcx ; ADCX r8,[rsi] ; ADOX r9,[rsi+8] ; ADCX r10,[rsi+8]
    check_native_parity({0x48, 0x01, 0xC8,
                         0x66, 0x4C, 0x0F, 0x38, 0xF6, 0x06,
                         0xF3, 0x4C, 0x0F, 0x38, 0xF6, 0x4E, 0x08,
                         0x66, 0x4C, 0x0F, 0x38, 0xF6, 0x56, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RCX, 1);
            cpu.gpr.set64(RSI, 0x2000);
            cpu.gpr.set64(R8, 1);
            cpu.gpr.set64(R9, 1);
            cpu.gpr.set64(R10, 1);
        });
}

// ADOX consumes and writes OF while leaving CF alone: STC's carry must survive.
TEST_F(JitParity64Test, AdoxPreservesCarryFromStc) {
    // STC ; ADOX edx, ebx  (F3 0F 38 F6 D3)
    check_parity({0xF9, 0xF3, 0x0F, 0x38, 0xF6, 0xD3},
        [](CpuState& cpu) {
            cpu.gpr.set64(RDX, 0xFFFFFFFFull);
            cpu.gpr.set64(RBX, 1);
        });
}

TEST_F(JitParity64Test, AdoxAfterAddPreservesCarry) {
    // ADD eax, ecx (carries) ; ADOX edx, ebx — CF must still read as set.
    check_parity({0x01, 0xC8, 0xF3, 0x0F, 0x38, 0xF6, 0xD3},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFull);
            cpu.gpr.set64(RCX, 1);
            cpu.gpr.set64(RDX, 0x10);
            cpu.gpr.set64(RBX, 0x20);
        });
}

TEST_F(JitParity64Test, Adox64AfterAdd) {
    check_parity({0x48, 0x01, 0xC8, 0xF3, 0x48, 0x0F, 0x38, 0xF6, 0xD3},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RCX, 1);
            cpu.gpr.set64(RDX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RBX, 1);
        });
}

// SHLD / SHRD with 32-bit operands *inside a 64-bit block*. These reach the
// x64 compiler, which used to have no case for them and delegated every one
// (128M executions on tools/jit_bench shld_hash). Beyond the shift itself the
// long-mode rule under test is that a 32-bit write zero-extends bits 63:32 of
// the destination.
TEST_F(JitParity64Test, Shld32InLongModeZeroExtendsDestination) {
    // SHLD EAX, EDX, 4 — 0F A4 D0 04
    check_native_parity({0x0F, 0xA4, 0xD0, 0x04},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFF89ABCDEFull);
            cpu.gpr.set64(RDX, 0xFFFFFFFF76543210ull);
        });
}

TEST_F(JitParity64Test, Shrd32InLongModeZeroExtendsDestination) {
    // SHRD EAX, EDX, 4 — 0F AC D0 04
    check_native_parity({0x0F, 0xAC, 0xD0, 0x04},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xDEADBEEF89ABCDEFull);
            cpu.gpr.set64(RDX, 0xCAFEBABE76543210ull);
        });
}

TEST_F(JitParity64Test, Shld32InLongModeCount1DefinesOverflow) {
    // SHLD EAX, EDX, 1 — the only count for which OF is architecturally defined.
    check_native_parity({0x0F, 0xA4, 0xD0, 0x01},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x0000000040000000ull);
            cpu.gpr.set64(RDX, 0x0000000080000000ull);
            cpu.flags.set_of(false);
        });
}

TEST_F(JitParity64Test, Shld32InLongModeCount0IsANoOp) {
    // A count of 0 must leave the destination and every flag untouched.
    check_native_parity({0x0F, 0xA4, 0xD0, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x1111111122222222ull);
            cpu.gpr.set64(RDX, 0x3333333344444444ull);
            cpu.flags.set_cf(true);
            cpu.flags.set_of(true);
            cpu.flags.set_zf(true);
        });
}

TEST_F(JitParity64Test, Shld32ByClInLongMode) {
    // SHLD EAX, EDX, CL — 0F A5 D0. The count is masked to 5 bits.
    check_native_parity({0x0F, 0xA5, 0xD0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFF0F0F0F0Full);
            cpu.gpr.set64(RDX, 0x00000000F0F0F0F0ull);
            cpu.gpr.set64(RCX, 12);
        });
}

TEST_F(JitParity64Test, Shrd32ByClCount0InLongModePreservesFlags) {
    // CL == 0 takes the no-op path, which must not touch flags either.
    check_native_parity({0x0F, 0xAD, 0xD0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x00000000ABCDEF01ull);
            cpu.gpr.set64(RDX, 0x000000001234567Aull);
            cpu.gpr.set64(RCX, 0);
            cpu.flags.set_cf(true);
        });
}

// A pending lazy FlagOp must not survive the double shift: SHLD redefines
// CF/SF/ZF/PF, so the XOR's flags have to be gone, not re-applied on top.
TEST_F(JitParity64Test, Shld32InLongModeDiscardsPendingLazyFlags) {
    // XOR ECX, ECX ; SHLD EAX, EDX, 8
    check_native_parity({0x31, 0xC9, 0x0F, 0xA4, 0xD0, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x00000000FF00FF00ull);
            cpu.gpr.set64(RDX, 0x0000000000FF00FFull);
        });
}

// 32-bit MUL/DIV inside a 64-bit block. compile_mul_div lowers these, but the
// 32-bit native chain is gated on !m_block_is_64bit, so they used to delegate
// (2.7% of all executed instructions each on an x64 int-division loop). The
// long-mode rule under test is that the 32-bit EAX/EDX writes zero-extend.
TEST_F(JitParity64Test, Div32InLongModeZeroExtendsQuotientAndRemainder) {
    // DIV ECX — F7 F1
    check_native_parity({0xF7, 0xF1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFF00BEEF01ull);
            cpu.gpr.set64(RDX, 0xFFFFFFFF00000002ull);
            cpu.gpr.set64(RCX, 0x00000000000000F7ull);
        });
}

TEST_F(JitParity64Test, Idiv32InLongModeNegativeDividend) {
    // IDIV ECX — F7 F9
    check_native_parity({0xF7, 0xF9},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFFF439EB2ull);  // EAX = -12345678
            cpu.gpr.set64(RDX, 0xFFFFFFFFFFFFFFFFull);  // EDX = sign extension
            cpu.gpr.set64(RCX, 0x0000000000000061ull);  // 97
        });
}

TEST_F(JitParity64Test, Mul32InLongModeWritesEdxEax) {
    // MUL ECX — F7 E1
    check_native_parity({0xF7, 0xE1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xAAAAAAAADEADBEEFull);
            cpu.gpr.set64(RDX, 0xBBBBBBBBCCCCCCCCull);
            cpu.gpr.set64(RCX, 0x0000000000001234ull);
        });
}

// IMUL r/m64 (48 F7 /5) — signed RAX * SRC -> RDX:RAX.
//
// This had no JIT lowering at all until 2026-09-09: every one delegated to the
// interpreter. It is the one-operand form a compiler emits for signed division
// by a constant, so it is everywhere in optimised code — .NET's clrjit runs it
// constantly. A wrong 128-bit product is silent data corruption, so the cases
// below pin the sign handling and the CF/OF rule specifically.
//
// CF/OF are set when the product does NOT sign-extend from the low 64 bits,
// i.e. high != (low >> 63). The unsigned test "high != 0" would report
// overflow for every negative result, which is the easy way to get this wrong.

TEST_F(JitParity64Test, Imul64OneOperandPositive) {
    check_native_parity({0x48, 0xF7, 0xE9},   // imul rcx
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x0000000012345678ull);
            cpu.gpr.set64(RDX, 0xDEADBEEFDEADBEEFull);  // must be overwritten
            cpu.gpr.set64(RCX, 0x000000000000002Bull);
        });
}

TEST_F(JitParity64Test, Imul64OneOperandNegativeSetsNoOverflow) {
    // -100 * 7 fits in 64 bits, so CF/OF must be CLEAR even though the high
    // half is all ones. This is the case an unsigned "high != 0" test fails.
    check_native_parity({0x48, 0xF7, 0xE9},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFF9Cull);  // -100
            cpu.gpr.set64(RDX, 0x1111111122222222ull);
            cpu.gpr.set64(RCX, 0x0000000000000007ull);
        });
}

TEST_F(JitParity64Test, Imul64OneOperandBothNegative) {
    check_native_parity({0x48, 0xF7, 0xE9},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFF9ABCDEF0ull);
            cpu.gpr.set64(RDX, 0x0ull);
            cpu.gpr.set64(RCX, 0xFFFFFFFFFFFFFFFEull);  // -2
        });
}

TEST_F(JitParity64Test, Imul64OneOperandOverflowsSetsCarry) {
    // A genuinely 128-bit-wide product: CF/OF must be SET.
    check_native_parity({0x48, 0xF7, 0xE9},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x4000000000000000ull);
            cpu.gpr.set64(RDX, 0x0ull);
            cpu.gpr.set64(RCX, 0x0000000000000004ull);
        });
}

TEST_F(JitParity64Test, Imul64OneOperandMinInt64Squared) {
    check_native_parity({0x48, 0xF7, 0xE9},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x8000000000000000ull);  // INT64_MIN
            cpu.gpr.set64(RDX, 0x0ull);
            cpu.gpr.set64(RCX, 0x8000000000000000ull);
        });
}

TEST_F(JitParity64Test, Imul64OneOperandByZero) {
    check_native_parity({0x48, 0xF7, 0xE9},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x123456789ABCDEF0ull);
            cpu.gpr.set64(RDX, 0xFFFFFFFFFFFFFFFFull);
            cpu.gpr.set64(RCX, 0x0ull);
        });
}

TEST_F(JitParity64Test, Imul64OneOperandSourceIsRaxItself) {
    // The multiplicand register is also the destination low half; the lowering
    // must read RAX before MUL overwrites it.
    check_native_parity({0x48, 0xF7, 0xE8},   // imul rax
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFFFBull);  // -5
            cpu.gpr.set64(RDX, 0xCCCCCCCCCCCCCCCCull);
        });
}

TEST_F(JitParity64Test, Imul32InLongModeSignedWidening) {
    // IMUL ECX — F7 E9
    check_native_parity({0xF7, 0xE9},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x00000000FFFFFF9Cull);  // EAX = -100
            cpu.gpr.set64(RDX, 0x1111111122222222ull);
            cpu.gpr.set64(RCX, 0x0000000000000007ull);
        });
}
