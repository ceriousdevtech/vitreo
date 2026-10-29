// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): shift/Jcc
// fusion.

#include "jit_parity_64_fixture.hpp"

// ---------------------------------------------------------------------------
// Shift → Jcc fusion (NzcvState::NzOnly)
//
// A nonzero-count immediate shift republishes only N/Z via TST: x86 CF is the
// shifted-out bit and OF is defined only for count==1 (Intel SDM Vol. 2,
// SAL/SAR/SHL/SHR), so only JZ/JNZ/JS/JNS may fuse; every other condition
// must fall back to the lazy record. Sweep all 16 conditions so a wrongly
// widened fusion (e.g. JB reading ARM C, which TST cleared) diverges.
// ---------------------------------------------------------------------------

namespace {

// Run a terminal-Jcc 64-bit block through both engines and require identical
// branch decision AND identical materialized flags (a fused branch must not
// corrupt the flags the lazy record owns).
void expect_shift_jcc_parity(GuestMemory& mem,
                             std::initializer_list<u8> code_il,
                             u64 a, u64 b,
                             u32 expect_insns) {
    JitCompiler jit(mem);
    std::vector<u8> code(code_il);
    ASSERT_TRUE(mem.write_bytes(0x1000, code.data(), code.size()));
    MemoryByteReader reader(code.data(), code.size());
    BasicBlock block;
    block.start_addr = 0x1000;
    block.is_64bit = true;
    while (reader.remaining() > 0) {
        DecodedInstruction inst;
        if (decode_instruction(reader, DecodeMode::Mode64, inst)
                != DecodeError::Success) {
            break;
        }
        block.instructions.push_back(inst);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    ASSERT_EQ(block.instruction_count, expect_insns);

    auto setup = [&](CpuState& c) {
        c.init_long_mode();
        c.gpr.set64(RSP, 0x10000);
        c.rip = 0x1000;
        c.gpr.set64(RAX, a);
        c.gpr.set64(RBX, b);
    };

    CpuState icpu;
    setup(icpu);
    {
        Interpreter interp(icpu, mem);
        (void)interp.run(block.instruction_count);
    }
    icpu.materialize_lazy_flags();

    CpuState jcpu;
    setup(jcpu);
    CompiledBlock* cb = jit.compile(block);
    ASSERT_NE(cb, nullptr);
    (void)test::execute_like_dispatcher(jit, *cb, jcpu);
    jcpu.materialize_lazy_flags();

    const auto ctx = [&](const char* what) {
        return ::testing::Message()
            << what << " diverged for a=0x" << std::hex << a << " b=0x" << b
            << " code[last]=0x" << static_cast<u32>(code[code.size() - 2]);
    };
    EXPECT_EQ(icpu.rip, jcpu.rip) << ctx("branch/RIP");
    EXPECT_EQ(icpu.flags.cf(), jcpu.flags.cf()) << ctx("CF");
    EXPECT_EQ(icpu.flags.zf(), jcpu.flags.zf()) << ctx("ZF");
    EXPECT_EQ(icpu.flags.sf(), jcpu.flags.sf()) << ctx("SF");
    EXPECT_EQ(icpu.flags.of(), jcpu.flags.of()) << ctx("OF");
    EXPECT_EQ(icpu.flags.af(), jcpu.flags.af()) << ctx("AF");
    EXPECT_EQ(icpu.flags.pf(), jcpu.flags.pf()) << ctx("PF");
}

constexpr u64 kShiftValues[] = {
    0,
    1,
    2,
    0x2000000000000000ull,  // SHL 3 -> 0: ZF=1 with CF=0
    0x3000000000000000ull,  // SHL 3 -> 1<<63: SF=1, CF=1
    0x1000000000000000ull,  // SHL 3 -> 1<<63: SF=1, CF=0
    0x8000000000000001ull,  // SHR 1: CF=1, result sign clear
    0xFFFFFFFFFFFFFFFFull,
};

}  // namespace

TEST_F(JitParity64Test, Shl64ImmJccAllConditionsParity) {
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (u64 v : kShiftValues) {
            // SHL RAX, 3 ; Jcc +2
            expect_shift_jcc_parity(
                *m_memory, {0x48, 0xC1, 0xE0, 0x03, jcc, 0x02}, v, 0, 2);
        }
    }
}

TEST_F(JitParity64Test, Shr64By1JccAllConditionsParity) {
    for (u8 jcc = 0x70; jcc <= 0x7F; ++jcc) {
        for (u64 v : kShiftValues) {
            // SHR RAX, 1 ; Jcc +2 — count==1, so OF is architecturally
            // defined too.
            expect_shift_jcc_parity(
                *m_memory, {0x48, 0xD1, 0xE8, jcc, 0x02}, v, 0, 2);
        }
    }
}

TEST_F(JitParity64Test, Shl64CountZeroLeavesFlagsAndBranchUntouched) {
    // ADD RAX, RBX ; SHL RAX, 0 ; JZ +2 — a masked-to-zero count leaves
    // every flag unchanged (Intel SDM Vol. 2), so the JZ must branch on the
    // ADD's ZF and the materialized flags must be the ADD's — no fusion
    // against a TST that never ran, no corruption of the pending record.
    constexpr std::pair<u64, u64> kAddPairs[] = {
        {1, 0xFFFFFFFFFFFFFFFFull},  // sum 0 -> taken
        {1, 2},                      // sum 3 -> not taken
        {0x123456789ABCDEFull, 0xFEDCBA9876543210ull},
    };
    for (auto [a, b] : kAddPairs) {
        expect_shift_jcc_parity(
            *m_memory,
            {0x48, 0x01, 0xD8, 0x48, 0xC1, 0xE0, 0x00, 0x74, 0x02},
            a, b, 3);
    }
}

TEST_F(JitParity64Test, Shl64JzEmitsFusedBranch) {
    // Reached-ness guard: the NzOnly publish must actually arm the fused
    // Jcc path, not silently fall back to the cond helper.
    vitreo::diagnostics::perf_window_enable_for_tests();
    vitreo::diagnostics::perf_window_start();
    const u8 code[] = {0x48, 0xC1, 0xE0, 0x03, 0x74, 0x02};
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    BasicBlock block = build_block(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 2u);
    ASSERT_NE(m_jit->compile(block), nullptr);
    const auto snap = vitreo::diagnostics::perf_window_snapshot();
    EXPECT_GE(snap.jcc_emit_fused, 1u);
    EXPECT_EQ(snap.jcc_emit_helper, 0u);
}

TEST_F(JitParity64Test, Sub32MemDestJccEmitsFusedBranchNotLazyWalk) {
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

    const u32 sub_only = compiled_size({0x83, 0x29, 0x01});
    const u32 fused = compiled_size({
        0x83, 0x29, 0x01, // SUB dword ptr [RCX],1
        0x75, 0x02,       // JNZ +2
        0x90, 0x90});

    // A native fused Jcc adds only a compact branch tail. The lazy-condition
    // evaluator adds hundreds of bytes, so this bound is deliberately loose
    // while still discriminating the two paths.
    EXPECT_LE(fused, sub_only + 256u)
        << "memory SUB -> Jcc retained the lazy-condition walk: sub="
        << sub_only << " fused=" << fused;
}

TEST_F(JitParity64Test, CmpThenSetccReadsFlagsAfterFusion) {
    // CMP RAX,RBX ; SETB DL (0F 92 C2) — SETB reads CF from the CMP, so the
    // lazy state must still be recorded even though a Jcc could have fused it.
    check_parity({0x48, 0x39, 0xD8, 0x0F, 0x92, 0xC2},
        [](CpuState& c) {
            c.gpr.set64(RAX, 3);
            c.gpr.set64(RBX, 5);
        },
        /*check_flags=*/true);
}

TEST_F(JitParity64Test, PcmpeqwMemUsesFull64BitAddress) {
    // EMIT_SSE_NEON_RR loads m128 as two guest qwords. A 32-bit ADD for the
    // high half truncates PartitionAlloc pointers (Chrome setup PCMPEQW AV:
    // [0x73a8…80090] → READ 0x80098). SetUp already maps [0x1000,0x11000).
    constexpr GuestAddr kHigh = 0x140001000ull;
    constexpr GuestAddr kTrunc = 0x1000ull;
    ASSERT_TRUE(m_memory->map_region(
        kHigh, 0x4000, MemoryProtection::ReadWrite).is_ok());
    const u8 high_bytes[16] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    };
    const u8 trunc_bytes[16] = {
        0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
        0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB,
    };
    ASSERT_TRUE(m_memory->write_bytes(kHigh, high_bytes, 16).is_ok());
    ASSERT_TRUE(m_memory->write_bytes(kTrunc, trunc_bytes, 16).is_ok());

    // PCMPEQW XMM0, [RAX] — high qword must come from kHigh+8, not kTrunc+8.
    check_native_parity({0x66, 0x0F, 0x75, 0x00},
        [high_bytes](CpuState& cpu) {
            cpu.gpr.set64(RAX, kHigh);
            for (int i = 0; i < 16; ++i) {
                cpu.sse.xmm[0].u8_[static_cast<usize>(i)] =
                    high_bytes[static_cast<usize>(i)];
            }
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}
