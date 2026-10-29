// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048):
// XCHG/XADD/CMPXCHG, ADC/SBB imm32 and LOCK atomics.

#include "jit_parity_64_fixture.hpp"

// ============================================================================
// Unlocked XCHG / XADD / CMPXCHG r64 + ADC imm32 (Intel SDM Vol. 2)
// ============================================================================

TEST_F(JitParity64Test, NativeXchgRm64R64Reg) {
    // XCHG RCX, RAX — 48 87 C1
    check_native_parity({0x48, 0x87, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x1111111111111111ull);
            cpu.gpr.set64(RCX, 0x2222222222222222ull);
        },
        false);
}

TEST_F(JitParity64Test, NativeXchgRm64R64Mem) {
    // XCHG [RAX], RCX — 48 87 08
    ASSERT_TRUE(m_memory->write_u64(0x3000, 0xAAAAAAAAAAAAAAAAull).is_ok());
    check_native_parity({0x48, 0x87, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.gpr.set64(RCX, 0xBBBBBBBBBBBBBBBBull);
        },
        false);
}

TEST_F(JitParity64Test, NativeXaddRm64R64Reg) {
    // XADD RAX, RCX — 48 0F C1 C8
    check_native_parity({0x48, 0x0F, 0xC1, 0xC8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x10);
            cpu.gpr.set64(RCX, 0x20);
            cpu.flags.rflags = 0x202;
        });
}

TEST_F(JitParity64Test, NativeXaddRm64R64Mem) {
    // XADD [RAX], RCX — 48 0F C1 08
    ASSERT_TRUE(m_memory->write_u64(0x3000, 0x100).is_ok());
    check_native_parity({0x48, 0x0F, 0xC1, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.gpr.set64(RCX, 0x23);
            cpu.flags.rflags = 0x202;
        });
}

TEST_F(JitParity64Test, NativeCmpxchgRm64R64Match) {
    // CMPXCHG RCX, RDX — 48 0F B1 D1 ; RAX==RCX → RCX:=RDX, ZF=1
    check_native_parity({0x48, 0x0F, 0xB1, 0xD1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x42);
            cpu.gpr.set64(RCX, 0x42);
            cpu.gpr.set64(RDX, 0x99);
            cpu.flags.rflags = 0x202;
        });
}

TEST_F(JitParity64Test, NativeCmpxchgRm64R64Mismatch) {
    // CMPXCHG RCX, RDX — 48 0F B1 D1 ; RAX!=RCX → RAX:=RCX, ZF=0
    check_native_parity({0x48, 0x0F, 0xB1, 0xD1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x42);
            cpu.gpr.set64(RCX, 0x77);
            cpu.gpr.set64(RDX, 0x99);
            cpu.flags.rflags = 0x202;
        });
}

TEST_F(JitParity64Test, NativeCmpxchgRm64R64MemMatch) {
    // CMPXCHG [RBX], RCX — 48 0F B1 0B
    ASSERT_TRUE(m_memory->write_u64(0x3000, 0x55).is_ok());
    check_native_parity({0x48, 0x0F, 0xB1, 0x0B},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x55);
            cpu.gpr.set64(RBX, 0x3000);
            cpu.gpr.set64(RCX, 0xAA);
            cpu.flags.rflags = 0x202;
        });
}

TEST_F(JitParity64Test, NativeCmpxchgRm64R64MemMismatch) {
    // CMPXCHG [RBX], RCX — 48 0F B1 0B ; RAX != [RBX] → RAX:=mem
    ASSERT_TRUE(m_memory->write_u64(0x3000, 0x55).is_ok());
    check_native_parity({0x48, 0x0F, 0xB1, 0x0B},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x11);
            cpu.gpr.set64(RBX, 0x3000);
            cpu.gpr.set64(RCX, 0xAA);
            cpu.flags.rflags = 0x202;
        });
}

TEST_F(JitParity64Test, NativeAdcRaxImm32WithCarry) {
    // ADC RAX, imm32 — 48 15 01 00 00 00
    check_native_parity({0x48, 0x15, 0x01, 0x00, 0x00, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFull);
            cpu.flags.rflags = 0x203;  // CF=1
        });
}

TEST_F(JitParity64Test, NativeAdcRm64Imm32Reg) {
    // ADC RCX, imm32 — 48 81 D1 EF BE AD 0E
    check_native_parity({0x48, 0x81, 0xD1, 0xEF, 0xBE, 0xAD, 0x0E},
        [](CpuState& cpu) {
            cpu.gpr.set64(RCX, 0x10);
            cpu.flags.rflags = 0x203;  // CF=1
        });
}

TEST_F(JitParity64Test, NativeAdcRm64Imm32Mem) {
    // ADC qword [RAX], imm32 — 48 81 10 02 00 00 00
    ASSERT_TRUE(m_memory->write_u64(0x3000, 0xFFFFFFFFFFFFFFFFull).is_ok());
    check_native_parity({0x48, 0x81, 0x10, 0x02, 0x00, 0x00, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.flags.rflags = 0x203;  // CF=1
        });
}

TEST_F(JitParity64Test, NativeAdcR64R64PublishesEveryArithmeticFlag) {
    // ADC RCX,RDX -- exercise the native ADCS lowering across AF/PF, carry,
    // zero, sign, and overflow boundaries. AF is opt-in because the legacy
    // parity helper intentionally did not compare it for unrelated opcodes.
    struct Case { u64 dst; u64 src; u64 rflags; };
    for (const Case& c : {
             Case{0x0Full, 0, 0x203},
             Case{0xFFFFFFFFFFFFFFFFull, 0, 0x203},
             Case{0x7FFFFFFFFFFFFFFFull, 0, 0x203},
             Case{0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull, 0x202},
         }) {
        check_native_parity({0x48, 0x13, 0xCA},
            [c](CpuState& cpu) {
                cpu.gpr.set64(RCX, c.dst);
                cpu.gpr.set64(RDX, c.src);
                cpu.flags.rflags = c.rflags;
            }, true, false, true);
    }
}

TEST_F(JitParity64Test, NativeAdcR64R64ContainsAdcs) {
    const u8 code[] = {0x48, 0x13, 0xCA};  // ADC RCX,RDX
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    ASSERT_NE(cb->code, nullptr);
    const auto* words = static_cast<const u32*>(cb->code);
    const usize count = cb->code_size / sizeof(u32);
    bool found_adcs = false;
    for (usize i = 0; i < count; ++i) {
        // ADCS X0,X0,X1 (the checked encoder word in stencils.hpp).
        found_adcs |= words[i] == 0xBA010000u;
    }
    EXPECT_TRUE(found_adcs) << "ADC r64 must stay in generated AArch64";
}

TEST_F(JitParity64Test, NativeSbbRaxImm32WithBorrow) {
    // SBB RAX, imm32 — 48 1D 01 00 00 00
    check_native_parity({0x48, 0x1D, 0x01, 0x00, 0x00, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x10);
            cpu.flags.rflags = 0x203;  // CF=1
        });
}

TEST_F(JitParity64Test, NativeSbbRm64Imm32Reg) {
    // SBB RCX, imm32 — 48 81 D9 EF BE AD 0E
    check_native_parity({0x48, 0x81, 0xD9, 0xEF, 0xBE, 0xAD, 0x0E},
        [](CpuState& cpu) {
            cpu.gpr.set64(RCX, 0x100000000ULL);
            cpu.flags.rflags = 0x203;  // CF=1
        });
}

TEST_F(JitParity64Test, NativeSbbRm64Imm32Mem) {
    // SBB qword [RAX], imm32 — 48 81 18 02 00 00 00
    ASSERT_TRUE(m_memory->write_u64(0x3000, 0x10).is_ok());
    check_native_parity({0x48, 0x81, 0x18, 0x02, 0x00, 0x00, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.flags.rflags = 0x203;  // CF=1
        });
}

// ============================================================================
// LOCK r/m64 atomics (Intel SDM Vol. 2 LOCK) — native helpers, no delegate
// ============================================================================

TEST_F(JitParity64Test, LockIncRm64) {
    // F0 48 FF 00 = LOCK INC QWORD PTR [RAX]
    ASSERT_TRUE(m_memory->write_u64(0x3000, 41).is_ok());
    check_native_parity({0xF0, 0x48, 0xFF, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.flags.set_cf(true);
        });
    EXPECT_EQ(m_memory->read_u64(0x3000).value_or(0), 42u);
}

TEST_F(JitParity64Test, LockIncRm32JneReadsResultFlags) {
    // Chromium's atomic refcount increment is exactly this boundary:
    //   lock inc dword ptr [rax]
    //   jne  normal_path
    // INC preserves CF but replaces ZF/SF/OF/AF/PF.  The locked helper writes
    // canonical RFLAGS, and the immediately following native Jcc must consume
    // those flags rather than host NZCV clobbered by the helper call.
    // The branch target is outside this block so both interpreter and JIT
    // execute exactly two guest instructions before returning to the caller.
    static const u8 code[] = {
        0xF0, 0xFF, 0x00,  // lock inc dword ptr [rax]
        0x75, 0xEB,        // jne 0x0ff0
    };

    for (u32 initial : {41u, 0xFFFFFFFFu}) {
        ASSERT_TRUE(m_memory->write_u32(0x3000, initial).is_ok());
        check_parity(code, sizeof(code), [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.flags.set_cf(true);
        }, /*check_flags=*/true, /*check_xmm=*/false,
           /*require_jit=*/true);
        EXPECT_EQ(m_memory->read_u32(0x3000).value_or(0), initial + 1u);
    }
}

TEST_F(JitParity64Test, LockDecRm64) {
    // F0 48 FF 08 = LOCK DEC QWORD PTR [RAX]
    ASSERT_TRUE(m_memory->write_u64(0x3000, 10).is_ok());
    check_native_parity({0xF0, 0x48, 0xFF, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.flags.set_cf(false);
        });
    EXPECT_EQ(m_memory->read_u64(0x3000).value_or(0), 9u);
}

TEST_F(JitParity64Test, LockXaddRm64R64) {
    // F0 48 0F C1 08 = LOCK XADD [RAX], RCX
    ASSERT_TRUE(m_memory->write_u64(0x3000, 0x100).is_ok());
    check_native_parity({0xF0, 0x48, 0x0F, 0xC1, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.gpr.set64(RCX, 0x23);
            cpu.flags.rflags = 0x202;
        });
    EXPECT_EQ(m_memory->read_u64(0x3000).value_or(0), 0x123u);
}

TEST_F(JitParity64Test, LockCmpxchgRm64R64Match) {
    // F0 48 0F B1 0B = LOCK CMPXCHG [RBX], RCX
    ASSERT_TRUE(m_memory->write_u64(0x3000, 0x55).is_ok());
    check_native_parity({0xF0, 0x48, 0x0F, 0xB1, 0x0B},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x55);
            cpu.gpr.set64(RBX, 0x3000);
            cpu.gpr.set64(RCX, 0xAA);
            cpu.flags.rflags = 0x202;
        });
    EXPECT_EQ(m_memory->read_u64(0x3000).value_or(0), 0xAAu);
}

TEST_F(JitParity64Test, LockCmpxchgRm64R64Mismatch) {
    ASSERT_TRUE(m_memory->write_u64(0x3000, 0x55).is_ok());
    check_native_parity({0xF0, 0x48, 0x0F, 0xB1, 0x0B},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x11);
            cpu.gpr.set64(RBX, 0x3000);
            cpu.gpr.set64(RCX, 0xAA);
            cpu.flags.rflags = 0x202;
        });
    EXPECT_EQ(m_memory->read_u64(0x3000).value_or(0), 0x55u);
}

TEST_F(JitParity64Test, LockXchgRm64R64) {
    // F0 48 87 08 = LOCK XCHG [RAX], RCX (F0 optional; mem XCHG is locked)
    ASSERT_TRUE(m_memory->write_u64(0x3000, 0xAAAAAAAAAAAAAAAAull).is_ok());
    check_native_parity({0xF0, 0x48, 0x87, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.gpr.set64(RCX, 0xBBBBBBBBBBBBBBBBull);
        },
        false);
    EXPECT_EQ(m_memory->read_u64(0x3000).value_or(0), 0xBBBBBBBBBBBBBBBBull);
}

TEST_F(JitParity64Test, ImplicitLockXchgRm64R64Mem) {
    // 48 87 08 — no F0; Intel SDM: XCHG mem still asserts LOCK#
    ASSERT_TRUE(m_memory->write_u64(0x3000, 0x1111111111111111ull).is_ok());
    check_native_parity({0x48, 0x87, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.gpr.set64(RCX, 0x2222222222222222ull);
        },
        false);
    EXPECT_EQ(m_memory->read_u64(0x3000).value_or(0), 0x2222222222222222ull);
}

TEST_F(JitParity64Test, LockOrAndXorAddSubRm64) {
    ASSERT_TRUE(m_memory->write_u64(0x3000, 0xF0F0F0F0F0F0F0F0ull).is_ok());
    // LOCK OR [RAX], RCX — F0 48 09 08
    check_native_parity({0xF0, 0x48, 0x09, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.gpr.set64(RCX, 0x0F0F0F0F0F0F0F0Full);
            cpu.flags.rflags = 0x202;
        });
    EXPECT_EQ(m_memory->read_u64(0x3000).value_or(0), 0xFFFFFFFFFFFFFFFFull);

    ASSERT_TRUE(m_memory->write_u64(0x3000, 0xFFFFFFFFFFFFFFFFull).is_ok());
    // LOCK AND [RAX], imm8 — F0 48 83 20 FE
    check_native_parity({0xF0, 0x48, 0x83, 0x20, 0xFE},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.flags.rflags = 0x202;
        });
    EXPECT_EQ(m_memory->read_u64(0x3000).value_or(0), 0xFFFFFFFFFFFFFFFEull);

    ASSERT_TRUE(m_memory->write_u64(0x3000, 0x1111).is_ok());
    // LOCK XOR [RAX], RCX — F0 48 31 08
    check_native_parity({0xF0, 0x48, 0x31, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.gpr.set64(RCX, 0x2222);
            cpu.flags.rflags = 0x202;
        });
    EXPECT_EQ(m_memory->read_u64(0x3000).value_or(0), 0x3333u);

    ASSERT_TRUE(m_memory->write_u64(0x3000, 100).is_ok());
    // LOCK ADD [RAX], RCX — F0 48 01 08
    check_native_parity({0xF0, 0x48, 0x01, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.gpr.set64(RCX, 7);
            cpu.flags.rflags = 0x202;
        });
    EXPECT_EQ(m_memory->read_u64(0x3000).value_or(0), 107u);

    ASSERT_TRUE(m_memory->write_u64(0x3000, 100).is_ok());
    // LOCK SUB [RAX], RCX — F0 48 29 08
    check_native_parity({0xF0, 0x48, 0x29, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.gpr.set64(RCX, 7);
            cpu.flags.rflags = 0x202;
        });
    EXPECT_EQ(m_memory->read_u64(0x3000).value_or(0), 93u);
}

TEST_F(JitParity64Test, LockNotNegRm64) {
    ASSERT_TRUE(m_memory->write_u64(0x3000, 0x0F0F0F0F0F0F0F0Full).is_ok());
    // LOCK NOT [RAX] — F0 48 F7 10
    check_native_parity({0xF0, 0x48, 0xF7, 0x10},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x3000); },
        false);
    EXPECT_EQ(m_memory->read_u64(0x3000).value_or(0), 0xF0F0F0F0F0F0F0F0ull);

    ASSERT_TRUE(m_memory->write_u64(0x3000, 5).is_ok());
    // LOCK NEG [RAX] — F0 48 F7 18
    check_native_parity({0xF0, 0x48, 0xF7, 0x18},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.flags.rflags = 0x202;
        });
    EXPECT_EQ(m_memory->read_u64(0x3000).value_or(0),
              static_cast<u64>(-5));
}

TEST_F(JitParity64Test, LockAdcSbbRm64) {
    ASSERT_TRUE(m_memory->write_u64(0x3000, 100).is_ok());
    // LOCK ADC [RAX], RCX — F0 48 11 08
    check_native_parity({0xF0, 0x48, 0x11, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.gpr.set64(RCX, 7);
            cpu.flags.rflags = 0x203;  // CF=1
        });
    EXPECT_EQ(m_memory->read_u64(0x3000).value_or(0), 108u);

    ASSERT_TRUE(m_memory->write_u64(0x3000, 100).is_ok());
    // LOCK SBB [RAX], imm8 — F0 48 83 18 07
    check_native_parity({0xF0, 0x48, 0x83, 0x18, 0x07},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.flags.rflags = 0x203;  // CF=1
        });
    EXPECT_EQ(m_memory->read_u64(0x3000).value_or(0), 92u);
}

TEST_F(JitParity64Test, LockBtBtsBtrBtcRm64) {
    ASSERT_TRUE(m_memory->write_u64(0x3000, 0).is_ok());
    // LOCK BTS [RAX], imm8=5 — F0 48 0F BA 28 05
    check_native_parity({0xF0, 0x48, 0x0F, 0xBA, 0x28, 0x05},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.flags.rflags = 0x202;
        });
    EXPECT_EQ(m_memory->read_u64(0x3000).value_or(0), 0x20u);

    // BT [RAX], imm8=5 — 48 0F BA 20 05 (CF from bit; no write).
    //
    // Deliberately NOT LOCK-prefixed. BT only reads, so it is absent from the
    // SDM's enumerated lockable set and `LOCK BT` is #UD on hardware; the
    // decoder now rejects it, where it used to decode.
    check_native_parity({0x48, 0x0F, 0xBA, 0x20, 0x05},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.flags.rflags = 0x202;
        });

    // LOCK BTR [RAX], RCX=5 — F0 48 0F B3 08
    check_native_parity({0xF0, 0x48, 0x0F, 0xB3, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.gpr.set64(RCX, 5);
            cpu.flags.rflags = 0x203;
        });
    EXPECT_EQ(m_memory->read_u64(0x3000).value_or(0), 0u);

    ASSERT_TRUE(m_memory->write_u64(0x3000, 0x20).is_ok());
    // LOCK BTC [RAX], imm8=5 — F0 48 0F BA 38 05
    check_native_parity({0xF0, 0x48, 0x0F, 0xBA, 0x38, 0x05},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x3000);
            cpu.flags.rflags = 0x202;
        });
    EXPECT_EQ(m_memory->read_u64(0x3000).value_or(0), 0u);
}
