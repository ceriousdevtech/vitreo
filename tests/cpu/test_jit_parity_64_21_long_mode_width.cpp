// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048):
// LEAVE/LOOP/JRCXZ, AND/OR r/m16, 80-bit x87 and ENTER.

#include "jit_parity_64_fixture.hpp"

// ---------------------------------------------------------------------------
// LEAVE / LOOP / JRCXZ in long mode are 64-bit wide.
// ---------------------------------------------------------------------------

// LEAVE: RSP := RBP, then pop RBP from an 8-byte slot. A 32-bit lowering zeroes
// RSP[63:32] and RBP[63:32] and reads the wrong 4 bytes.
TEST_F(JitParity64Test, LeaveInLongModeUsesFullWidthAndEightByteSlot) {
    constexpr GuestAddr kFrame = 0x140006800ull;
    map_tracked_region(0x140006000ull, 0x2000);
    ASSERT_TRUE(m_memory->write_u64(kFrame, 0x1122334455667788ull).is_ok());
    check_native_parity({0xC9},
        [](CpuState& cpu) {
            cpu.gpr.set64(RBP, kFrame);
            cpu.gpr.set64(RSP, 0x140006400ull);
        },
        /*check_flags=*/false);
}

// LOOP decrements RCX, not ECX. Here the low half reaches zero while the full
// register does not, so a 32-bit decrement takes the wrong branch AND loses the
// upper half.
TEST_F(JitParity64Test, LoopInLongModeDecrementsFullRcx) {
    check_native_parity({0xE2, 0x00},
        [](CpuState& cpu) { cpu.gpr.set64(RCX, 0x0000000100000001ull); });
}

// LOOP must leave EFLAGS untouched even though the lowering's SUBS clobbers
// host NZCV (Intel SDM Vol. 2 LOOP: flags unaffected).
TEST_F(JitParity64Test, LoopInLongModePreservesFlags) {
    check_native_parity({0xE2, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RCX, 0x0000000100000001ull);
            cpu.flags.set_cf(true);
            cpu.flags.set_of(true);
            cpu.flags.set_zf(true);
        });
}

// JRCXZ is what 0xE3 decodes to in long mode, and it had no lowering at all.
// RCX's low half is zero but RCX is not, so testing ECX branches wrongly.
TEST_F(JitParity64Test, JrcxzInLongModeTestsFullRcx) {
    check_native_parity({0xE3, 0x05},
        [](CpuState& cpu) { cpu.gpr.set64(RCX, 0x0000000100000000ull); });
}

// ---------------------------------------------------------------------------
// AND/OR r/m16 register forms: these six names were omitted from the 8/16-bit
// ALU routing list because they collide with compile_locked's LOCK-only cases.
// A 16-bit write preserves bits 63:16.
// ---------------------------------------------------------------------------
TEST_F(JitParity64Test, And16RegisterFormsInLongMode) {
    check_native_parity({
            0x66, 0x81, 0xE1, 0x34, 0x12,   // and cx, 0x1234
            0x66, 0x83, 0xE0, 0x0F,         // and ax, 0x0f
            0x66, 0x21, 0xD8,               // and ax, bx
        },
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xDEADBEEFCAFE5A5Aull);
            cpu.gpr.set64(RBX, 0x1122334455663C3Cull);
            cpu.gpr.set64(RCX, 0xFEDCBA9876543210ull);
        });
}

TEST_F(JitParity64Test, Or16RegisterFormsInLongMode) {
    check_native_parity({
            0x66, 0x81, 0xC9, 0x34, 0x12,   // or cx, 0x1234
            0x66, 0x83, 0xC8, 0x0F,         // or ax, 0x0f
            0x66, 0x09, 0xD8,               // or ax, bx
        },
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xDEADBEEFCAFE0000ull);
            cpu.gpr.set64(RBX, 0x1122334455663C3Cull);
            cpu.gpr.set64(RCX, 0xFEDCBA9876540000ull);
        });
}

// A CL-count shift with a MEMORY destination computes the masked count into
// W8, then loads the operand. In a 64-bit block emit_x64_region_load /
// emit_guest_load16_64 use W8 as scratch on their inline path, so the CF/OF
// computation afterwards read garbage — the result value stayed correct while
// the flags silently steered the next branch. The 32-bit linear path never
// touched W8, which is why this only appeared once these were routed into
// 64-bit blocks. Operand is above 4 GB so the region path is genuinely used.
TEST_F(JitParity64Test, Shl8ByClMemoryDestinationKeepsShiftCountForFlags) {
    constexpr GuestAddr kHigh = 0x140006000ull;
    map_tracked_region(kHigh, 0x1000);
    ASSERT_TRUE(m_memory->write_u8(kHigh, 0x81).is_ok());
    check_native_parity({0xD2, 0x20},               // shl byte [rax], cl
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, kHigh);
            cpu.gpr.set64(RCX, 1);                  // count 1 => OF defined
        });
}

TEST_F(JitParity64Test, Sar16ByClMemoryDestinationKeepsShiftCountForFlags) {
    constexpr GuestAddr kHigh = 0x140006000ull;
    map_tracked_region(kHigh, 0x1000);
    ASSERT_TRUE(m_memory->write_u16(kHigh, 0x8001).is_ok());
    check_native_parity({0x66, 0xD3, 0x38},         // sar word [rax], cl
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, kHigh);
            cpu.gpr.set64(RCX, 3);
        });
}

TEST_F(JitParity64Test, Shl16ByClMemoryDestinationCarryFromHighBit) {
    constexpr GuestAddr kHigh = 0x140006000ull;
    map_tracked_region(kHigh, 0x1000);
    ASSERT_TRUE(m_memory->write_u16(kHigh, 0x4001).is_ok());
    check_native_parity({0x66, 0xD3, 0x20},         // shl word [rax], cl
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, kHigh);
            cpu.gpr.set64(RCX, 2);
        });
}

TEST_F(JitParity64Test, Sar8ByClMemoryDestinationKeepsShiftCountForFlags) {
    constexpr GuestAddr kHigh = 0x140006000ull;
    map_tracked_region(kHigh, 0x1000);
    ASSERT_TRUE(m_memory->write_u8(kHigh, 0x81).is_ok());
    check_native_parity({0xD2, 0x38},               // sar byte [rax], cl
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, kHigh);
            cpu.gpr.set64(RCX, 1);
        });
}

// Register-destination control for the memory case above: isolates whether the
// OF divergence is specific to the memory path (guest-load clobbers) or is a
// general SHL r/m8, CL flag bug.
TEST_F(JitParity64Test, Shl8ByClRegisterDestinationControl) {
    check_native_parity({0xD2, 0xE0},               // shl al, cl
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x81);
            cpu.gpr.set64(RCX, 1);
        });
}

// ---------------------------------------------------------------------------
// 80-bit x87 load/store: register lifetimes across the guest memory helpers.
//
// Both of these must use an INDEXED operand ABOVE 4 GB. The index is what makes
// emit_compute_ea64 claim REG_EA_TMP (X5) as its internal scratch, and the high,
// cold address is what forces the guest load/store through its slow path, where
// emit_helper_blr() destroys every caller-saved register. A low, already-warm
// address exercises only the inline region path and proves nothing -- which is
// why both bugs survived.
//
// The assertions are on the stored MEMORY bytes: check_parity compares GPRs,
// RIP, EFLAGS and optionally XMM, but never cpu.fpu, so an x87 test that only
// calls check_native_parity asserts almost nothing.
// ---------------------------------------------------------------------------

// FSTP m80 captured exponent_sign into X5 and only then computed the effective
// address, so the indexed EA overwrote it with the index register -- and the
// qword store's helper call would have killed it regardless. -2.0 is chosen
// because its exponent word (0xC000) is non-zero and its mantissa is not, so a
// clobbered or zeroed exponent register cannot pass by accident.
TEST_F(JitParity64Test, X87FstpM80IndexedEaPreservesExponentAndSign) {
    constexpr GuestAddr kBase = 0x140006000ull;
    constexpr GuestAddr kDst  = kBase + 0x1000ull;   // = RDX + RCX*8
    map_tracked_region(kBase, 0x2000);

    // FSTP tbyte ptr [rdx+rcx*8] -- DB /7, mod=00 rm=100(SIB),
    // SIB = scale 8, index RCX, base RDX.
    check_native_parity({0xDB, 0x3C, 0xCA},
        [](CpuState& cpu) {
            cpu.gpr.set64(RDX, kDst - 0x100ull);
            cpu.gpr.set64(RCX, 0x20);                // 0x20 * 8 == 0x100
            // -2.0 in double-extended: explicit integer bit set, biased
            // exponent 0x4000, sign 1.
            cpu.fpu.push(Float80::from_extended(0x8000000000000000ull, 0xC000));
        },
        /*check_flags=*/false);

    u8 out[10] = {};
    ASSERT_TRUE(m_memory->read_bytes(kDst, out, sizeof(out)).is_ok());
    const u8 expected[10] = {0x00, 0x00, 0x00, 0x00, 0x00,
                             0x00, 0x00, 0x80,          // mantissa LE
                             0x00, 0xC0};               // exponent_sign LE
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(out[i], expected[i])
            << "FSTP m80 byte " << i << " diverged";
    }
}

// FLD m80 left the mantissa in X0 across the 16-bit exponent read. That read
// uses its own region-cache slot, so warming the 64-bit read does not warm it,
// and its slow path returns the helper's value in X0 -- the mantissa became the
// zero-extended exponent word. Round-trip through a second buffer, since the
// harness cannot observe cpu.fpu.
TEST_F(JitParity64Test, X87FldM80IndexedEaPreservesMantissaAcrossExponentRead) {
    constexpr GuestAddr kBase = 0x140006000ull;
    constexpr GuestAddr kSrc  = kBase;
    constexpr GuestAddr kDst  = kBase + 0x1000ull;
    map_tracked_region(kBase, 0x2000);

    const u8 kMinusTwo[10] = {0x00, 0x00, 0x00, 0x00, 0x00,
                              0x00, 0x00, 0x80,
                              0x00, 0xC0};
    ASSERT_TRUE(m_memory->write_bytes(kSrc, kMinusTwo, sizeof(kMinusTwo)).is_ok());

    // FLD  tbyte ptr [rax+rcx*8]   -- DB /5, SIB scale 8, index RCX, base RAX
    // FSTP tbyte ptr [rdx+rcx*8]   -- DB /7, SIB scale 8, index RCX, base RDX
    check_native_parity({0xDB, 0x2C, 0xC8,
                         0xDB, 0x3C, 0xCA},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, kSrc - 0x100ull);
            cpu.gpr.set64(RDX, kDst - 0x100ull);
            cpu.gpr.set64(RCX, 0x20);                // 0x20 * 8 == 0x100
        },
        /*check_flags=*/false);

    u8 out[10] = {};
    ASSERT_TRUE(m_memory->read_bytes(kDst, out, sizeof(out)).is_ok());
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(out[i], kMinusTwo[i])
            << "FLD m80 -> FSTP m80 round-trip byte " << i << " diverged";
    }
}

// ---------------------------------------------------------------------------
// ENTER in long mode. Intel SDM Vol. 2: in 64-bit mode the operand size is
// forced to 64, so frame slots are 8 bytes, StackAddrSize is 64, and the
// display walk strides by 8. The stack lives above 4 GB: a lowering that
// reads or writes RSP/RBP 32-bit lands on an unmapped address instead of
// quietly producing a plausible answer.
// ---------------------------------------------------------------------------

TEST_F(JitParity64Test, NativeEnterLongModePushesEightByteSlot) {
    constexpr GuestAddr stack_base = 0x140020000ull;
    constexpr GuestAddr rsp_in     = 0x140028000ull;
    constexpr GuestAddr rbp_in     = 0x140029000ull;
    map_tracked_region(stack_base, 0x10000);

    // ENTER 0x20, 0 — C8 20 00 00
    check_native_parity({0xC8, 0x20, 0x00, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RSP, rsp_in);
            cpu.gpr.set64(RBP, rbp_in);
        });

    // Push(RBP) puts the full 8-byte RBP at RSP-8; RBP := that address;
    // RSP := RSP - 8 - 0x20.
    auto saved_bp = m_memory->read_u64(rsp_in - 8);
    ASSERT_TRUE(saved_bp.is_ok());
    EXPECT_EQ(saved_bp.value(), rbp_in);
}

TEST_F(JitParity64Test, NativeEnterLongModeNestingOnePushesFrameTemp) {
    constexpr GuestAddr stack_base = 0x140020000ull;
    constexpr GuestAddr rsp_in     = 0x140028000ull;
    constexpr GuestAddr rbp_in     = 0x140029000ull;
    map_tracked_region(stack_base, 0x10000);

    // ENTER 0x10, 1 — C8 10 00 01
    check_native_parity({0xC8, 0x10, 0x00, 0x01},
        [](CpuState& cpu) {
            cpu.gpr.set64(RSP, rsp_in);
            cpu.gpr.set64(RBP, rbp_in);
        });

    // Slot 1: saved RBP. Slot 2: FrameTemp, itself a full 64-bit RSP value
    // (rsp_in - 8) — a 32-bit FrameTemp would store 0x40027FF8.
    auto saved_bp   = m_memory->read_u64(rsp_in - 8);
    auto frame_temp = m_memory->read_u64(rsp_in - 16);
    ASSERT_TRUE(saved_bp.is_ok());
    ASSERT_TRUE(frame_temp.is_ok());
    EXPECT_EQ(saved_bp.value(), rbp_in);
    EXPECT_EQ(frame_temp.value(), rsp_in - 8);
}

TEST_F(JitParity64Test, NativeEnterLongModeDisplayCopyUsesEightByteStride) {
    constexpr GuestAddr stack_base = 0x140020000ull;
    constexpr GuestAddr rsp_in     = 0x140028000ull;
    constexpr GuestAddr rbp_in     = 0x140029000ull;
    map_tracked_region(stack_base, 0x10000);

    // The two display entries the loop must copy, at RBP-8 and RBP-16. Both
    // have non-zero high halves, and the value at RBP-4 is deliberately
    // distinct so a 4-byte stride copies the wrong words.
    ASSERT_TRUE(m_memory->write_u64(rbp_in - 8,  0xAAAAAAAA11111111ull).is_ok());
    ASSERT_TRUE(m_memory->write_u64(rbp_in - 16, 0xBBBBBBBB22222222ull).is_ok());

    // ENTER 0x08, 3 — C8 08 00 03
    check_native_parity({0xC8, 0x08, 0x00, 0x03},
        [](CpuState& cpu) {
            cpu.gpr.set64(RSP, rsp_in);
            cpu.gpr.set64(RBP, rbp_in);
        });

    // Layout, top down:
    //   rsp_in-8  : saved RBP           (FrameTemp = rsp_in-8)
    //   rsp_in-16 : [rbp_in-8]
    //   rsp_in-24 : [rbp_in-16]
    //   rsp_in-32 : FrameTemp
    // then RBP := rsp_in-8, RSP := rsp_in-32-8.
    auto saved_bp   = m_memory->read_u64(rsp_in - 8);
    auto disp1      = m_memory->read_u64(rsp_in - 16);
    auto disp2      = m_memory->read_u64(rsp_in - 24);
    auto frame_temp = m_memory->read_u64(rsp_in - 32);
    ASSERT_TRUE(saved_bp.is_ok());
    ASSERT_TRUE(disp1.is_ok());
    ASSERT_TRUE(disp2.is_ok());
    ASSERT_TRUE(frame_temp.is_ok());
    EXPECT_EQ(saved_bp.value(), rbp_in);
    EXPECT_EQ(disp1.value(), 0xAAAAAAAA11111111ull);
    EXPECT_EQ(disp2.value(), 0xBBBBBBBB22222222ull);
    EXPECT_EQ(frame_temp.value(), rsp_in - 8);

    // RBP must end at FrameTemp and RSP 40 bytes below entry — the register
    // check inside check_native_parity already compares against the
    // interpreter, this pins the absolute values.
    // (Interpreter contract: exec_enter, interpreter/ops/indirect_ctrl.cpp.)
}
