// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): CMP/ADD,
// flag-neutral MOV, Jcc fusion.

#include "jit_parity_64_fixture.hpp"

// ============================================================================
// CMP/ADD -> flag-neutral MOV -> Jcc fusion (64-bit blocks)
//
// Intel SDM Vol. 2 MOV: "Flags Affected: None". A 64-bit block used to drop
// the compare's host NZCV across any MOV, so the Jcc fell back to the full
// emit_eval_lazy_condition walk. These tests pin both halves: the branch must
// still be architecturally correct on BOTH edges, and the emitted block must
// actually be the short (fused) form.
// ============================================================================

TEST_F(JitParity64Test, NativeCmpStoreJzFusesAndStaysCorrectNotTaken) {
    map_tracked_region(0x30000, 0x1000);
    // CMP EAX, 0x7FFFFFFF   3D FF FF FF 7F
    // MOV [RCX+8], EAX      89 41 08
    // JZ +2                 74 02
    // ADD EAX, 1            83 C0 01
    check_native_parity({
            0x3D, 0xFF, 0xFF, 0xFF, 0x7F,
            0x89, 0x41, 0x08,
            0x74, 0x02,
            0x83, 0xC0, 0x01,
        },
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x00000042ull);   // != imm -> not taken
            cpu.gpr.set64(RCX, 0x30000);
        });
    const auto stored = m_memory->read_u32(0x30008);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0x42u);
}

TEST_F(JitParity64Test, NativeCmpStoreJzFusesAndStaysCorrectTaken) {
    map_tracked_region(0x30000, 0x1000);
    check_native_parity({
            0x3D, 0xFF, 0xFF, 0xFF, 0x7F,
            0x89, 0x41, 0x08,
            0x74, 0x02,
            0x83, 0xC0, 0x01,
        },
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x7FFFFFFFull);   // == imm -> taken
            cpu.gpr.set64(RCX, 0x30000);
        });
}

// ADDS leaves ARM C == x86 CF (no inversion), so JB after ADD;MOV must fuse to
// B.CS, not B.CC. fused_arm_cond(NzcvState::Add, 0x2) encodes that; this test
// is what catches a copy/paste of the subtract map.
TEST_F(JitParity64Test, NativeAddStoreJbUsesAddCarrySense) {
    map_tracked_region(0x30000, 0x1000);
    // ADD EAX, 1        83 C0 01
    // MOV [RCX+8], EAX  89 41 08
    // JB +2             72 02
    // ADD EAX, 0x10     83 C0 10
    for (u64 seed : {0xFFFFFFFFull, 0x00000001ull}) {
        check_native_parity({
                0x83, 0xC0, 0x01,
                0x89, 0x41, 0x08,
                0x72, 0x02,
                0x83, 0xC0, 0x10,
            },
            [seed](CpuState& cpu) {
                cpu.gpr.set64(RAX, seed);
                cpu.gpr.set64(RCX, 0x30000);
            });
    }
}

// CLC installs a CF overlay over a live FlagOp by design. The overlay gate at
// the top of emit_eval_lazy_condition must still win, and CLC must still kill
// the fusable NZCV kind so JB never branches on the compare's carry.
TEST_F(JitParity64Test, NativeCmpClcStoreJbHonoursCfOverlay) {
    map_tracked_region(0x30000, 0x1000);
    // CMP EAX, 0x7FFFFFFF  3D FF FF FF 7F
    // CLC                  F8
    // MOV [RCX+8], EAX     89 41 08
    // JB +2                72 02
    // ADD EAX, 1           83 C0 01
    check_native_parity({
            0x3D, 0xFF, 0xFF, 0xFF, 0x7F,
            0xF8,
            0x89, 0x41, 0x08,
            0x72, 0x02,
            0x83, 0xC0, 0x01,
        },
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x00000001ull);   // CMP would set CF; CLC clears
            cpu.gpr.set64(RCX, 0x30000);
        });
}

// MOV r32,[mem] must NOT be treated as NZCV-preserving: emit_x64_region_load
// bounds-checks with SUBS and does not spill/fill. This is the recorded
// ADD;MOV r,[mem];ADC carry-drop shape.
TEST_F(JitParity64Test, NativeAddLoadAdcDoesNotTrustClobberedNzcv) {
    map_tracked_region(0x30000, 0x1000);
    ASSERT_TRUE(m_memory->write_u32(0x2010, 0x11223344u).is_ok());
    // ADD EAX, ECX       01 C8
    // MOV ECX, [RBX+16]  8B 4B 10
    // ADC ESI, 0         83 D6 00
    check_native_parity({
            0x01, 0xC8,
            0x8B, 0x4B, 0x10,
            0x83, 0xD6, 0x00,
        },
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFull);
            cpu.gpr.set64(RCX, 0x00000001ull);   // carry out of the ADD
            cpu.gpr.set64(RBX, 0x30000);
            cpu.gpr.set64(RSI, 0x00000000ull);
        });
}

// The optimisation itself: with fusion restored the Jcc is one B.cond, so the
// three-instruction block must not be materially larger than the same block
// without the intervening store. Before the fix the MOV variant carries the
// whole ~230-instruction emit_eval_lazy_condition walk.
TEST_F(JitParity64Test, NativeCmpStoreJccEmitsFusedBranchNotLazyWalk) {
    map_tracked_region(0x30000, 0x1000);
    auto compiled_size = [&](std::initializer_list<u8> code) -> u32 {
        std::vector<u8> v(code);
        EXPECT_TRUE(m_memory->write_bytes(0x1000, v.data(), v.size()));
        BasicBlock block = build_block(v.data(), v.size());
        EXPECT_GT(block.instruction_count, 0u);
        CompiledBlock* cb = m_jit->compile(block);
        EXPECT_NE(cb, nullptr);
        return cb ? cb->code_size : 0u;
    };
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");

    // CMP EAX,imm32 ; JZ +2 ; ADD EAX,1   (already fuses today)
    const u32 fused = compiled_size({
        0x3D, 0xFF, 0xFF, 0xFF, 0x7F, 0x74, 0x02, 0x83, 0xC0, 0x01});
    // MOV [RCX+8], EAX                    (cost of the store alone)
    const u32 store_only = compiled_size({0x89, 0x41, 0x08});
    // CMP EAX,imm32 ; MOV [RCX+8],EAX ; JZ +2 ; ADD EAX,1
    const u32 with_store = compiled_size({
        0x3D, 0xFF, 0xFF, 0xFF, 0x7F, 0x89, 0x41, 0x08,
        0x74, 0x02, 0x83, 0xC0, 0x01});

    // Generous bound: the store's own code plus 16 spare instructions. The
    // lazy-condition walk is ~230 instructions (~920 bytes), so this fails
    // by a wide margin without the fix and passes with plenty of headroom
    // once the Jcc fuses.
    EXPECT_LE(with_store, fused + store_only + 64u)
        << "CMP -> MOV [mem] -> Jcc did not fuse: fused=" << fused
        << " store_only=" << store_only << " with_store=" << with_store;
}
