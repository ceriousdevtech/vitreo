// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048)
//
// Extends parity coverage to 64-bit mode with REX prefixes, RIP-relative
// addressing, and 64-bit register operations.
// Fixture: jit_parity_64_fixture.hpp. More tests: test_jit_parity_64_*.cpp.

#include "jit_parity_64_fixture.hpp"

// The 64-bit ADC/SBB r/m32, r32 chain fast path (an Add/Sub-kind flag
// producer right before it) built its lazy record's source from the WHOLE
// pinned register: RAX's upper half reached the record's CF field (bit 32),
// so CF/AF/OF materialized from the record were wrong. Found by the tier-2
// T14 flags-live sweep (a pooled R8-R15 source has the same shape).
TEST_F(JitParity64Test, AdcSbbR32ChainRecordTakesTheZeroExtendedSource) {
    for (const u8 op : {u8{0x11}, u8{0x19}}) {  // adc / sbb ecx, eax
        SCOPED_TRACE(op);
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        const u8 code[] = {
            0x01, 0xD9,  // add ecx, ebx
            op, 0xC1,    // adc|sbb ecx, eax
        };
        for (const u64 rax : {0xFFFFFFFF00000001ull, 0x0000000100000000ull,
                              0x8000000080000000ull}) {
            SCOPED_TRACE(rax);
            check_parity(code, sizeof(code), [&](CpuState& cpu) {
                cpu.gpr.rax = rax;
                cpu.gpr.rcx = 0xFFFFFFF0u;
                cpu.gpr.rbx = 0x20;
            }, /*check_flags=*/true, /*check_xmm=*/false,
               /*require_jit=*/true, /*check_af=*/true);
        }
    }
}

TEST_F(JitParity64Test, Vex256ByteShuffleAllMasksAndRegisterAliasesNatively) {
    for (u8 dst : {u8{0}, u8{1}, u8{8}}) {
        for (u8 data : {u8{0}, u8{1}, u8{8}}) {
            for (u8 mask : {u8{0}, u8{1}, u8{8}}) {
                for (unsigned batch = 0; batch < 8; ++batch) {
                    m_jit = std::make_unique<JitCompiler>(*m_memory);
                    const u8 ext = static_cast<u8>(0x42 |
                        (dst < 8 ? 0x80 : 0) | (mask < 8 ? 0x20 : 0));
                    const u8 vex = static_cast<u8>(((~data & 15) << 3) | 5);
                    const u8 modrm = static_cast<u8>(0xC0 | ((dst & 7) << 3) | (mask & 7));
                    SCOPED_TRACE(::testing::Message() << "dst=" << +dst
                        << " data=" << +data << " mask=" << +mask << " batch=" << batch);
                    check_native_parity({0xC4, ext, vex, 0x00, modrm},
                        [&](CpuState& cpu) {
                            for (u8 i = 0; i < 16; ++i) {
                                cpu.sse.xmm[data].u8_[i] = 0x20 + i;
                                cpu.avx.ymm_high[data].u8_[i] = 0x60 + i;
                            }
                            for (u8 i = 0; i < 16; ++i) {
                                cpu.sse.xmm[mask].u8_[i] = batch * 32 + i;
                                cpu.avx.ymm_high[mask].u8_[i] = batch * 32 + 16 + i;
                            }
                        }, true, true);
                    ASSERT_TRUE(m_last_parity_ok);
                }
            }
        }
    }
}

TEST_F(JitParity64Test, Vex256PackedConversionsMatchBothLanesAndRounding) {
    for (u8 pp : {u8{0}, u8{1}, u8{2}}) {
        for (u32 rounding = 0; rounding < 4; ++rounding) {
            for (u8 dst : {u8{0}, u8{1}, u8{8}}) {
                const u8 src = dst == 8 ? 8 : 1;
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                const u8 vex = static_cast<u8>(0xFC | pp);
                const u8 extension = static_cast<u8>(0x41 | (dst < 8 ? 0x80 : 0) | (src < 8 ? 0x20 : 0));
                const u8 modrm = static_cast<u8>(0xC0 | ((dst & 7) << 3) | (src & 7));
                SCOPED_TRACE(::testing::Message() << "pp=" << +pp
                    << " rc=" << rounding << " dst=" << +dst);
                check_native_parity({0xC4, extension, static_cast<u8>(vex & 0x7F), 0x5B, modrm},
                    [&](CpuState& cpu) {
                        cpu.sse.mxcsr = 0x1F80 | (rounding << 13);
                        const u32 integers[] = {0, 1, 0xFFFFFFFF, 16777217,
                            0x7FFFFFFF, 0x80000000, 0xFEFFFFFF, 0x12345678};
                        const float floats[] = {0.5f, 1.5f, -0.5f, -1.5f,
                            2147483648.0f, -2147483648.0f,
                            std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity()};
                        for (u8 lane = 0; lane < 8; ++lane) {
                            auto& half = lane < 4 ? cpu.sse.xmm[src] : cpu.avx.ymm_high[src];
                            if (pp == 0) half.u32_[lane & 3] = integers[lane];
                            else half.f32_[lane & 3] = floats[lane];
                        }
                    }, true, true);
                EXPECT_EQ(m_last_jit_cpu.sse.mxcsr, m_last_interp_cpu.sse.mxcsr);
            }
        }
    }
}

TEST_F(JitParity64Test, VexScalarMovesMergeEveryRegisterAliasNatively) {
    for (u8 pp : {u8{2}, u8{3}}) {
        for (u8 dst = 0; dst < 3; ++dst) {
            for (u8 merge = 0; merge < 3; ++merge) {
                for (u8 src = 0; src < 3; ++src) {
                    // Each encoding is a separate program, not repeated
                    // self-modification of one compiler's guest address.
                    m_jit = std::make_unique<JitCompiler>(*m_memory);
                    const u8 vex = static_cast<u8>(0x80 | ((~merge & 15) << 3) | pp);
                    const u8 modrm = static_cast<u8>(0xC0 | (dst << 3) | src);
                    SCOPED_TRACE(::testing::Message() << "pp=" << +pp
                        << " dst=" << +dst << " merge=" << +merge << " src=" << +src);
                    check_native_parity({0xC5, vex, 0x10, modrm},
                        [](CpuState& cpu) {
                            for (u8 r = 0; r < 16; ++r) {
                                cpu.sse.xmm[r].u64_[0] = 0xAABBCCDD12345678ull + r;
                                cpu.sse.xmm[r].u64_[1] = 0xFEDCBA9876543210ull + r;
                                cpu.avx.ymm_high[r].u64_[0] = 0x1111222233334444ull;
                                cpu.avx.ymm_high[r].u64_[1] = 0x5555666677778888ull;
                            }
                        }, true, true);
                }
            }
        }
    }
}

TEST_F(JitParity64Test, VexScalarLoadsUse64BitAddressAndZeroUpperBits) {
    constexpr GuestAddr data = 0x100004000ull;
    map_tracked_region(data, 0x1000);
    ASSERT_TRUE(m_memory->write_u64(data, 0x7FF01234ABCDEF01ull));
    for (u8 vex : {u8{0xFA}, u8{0xFB}}) {
        check_native_parity({0xC5, vex, 0x10, 0x08},
            [&](CpuState& cpu) {
                cpu.gpr.set64(RAX, data);
                cpu.sse.xmm[1].u64_[0] = ~u64{0};
                cpu.sse.xmm[1].u64_[1] = ~u64{0};
                cpu.avx.ymm_high[1].u64_[0] = ~u64{0};
                cpu.avx.ymm_high[1].u64_[1] = ~u64{0};
            }, true, true);
    }
}

TEST_F(JitParity64Test, VexScalarLoadFaultPreservesDestination) {
    for (u8 vex : {u8{0xFA}, u8{0xFB}}) {
        check_native_parity({0xC5, vex, 0x10, 0x08},
            [](CpuState& cpu) {
                cpu.gpr.set64(RAX, 0x123450000ull);
                cpu.sse.xmm[1].u64_[0] = 0x12345678;
                cpu.sse.xmm[1].u64_[1] = 0x87654321;
                cpu.avx.ymm_high[1].u64_[0] = 0xAABBCCDD;
                cpu.avx.ymm_high[1].u64_[1] = 0xDDCCBBAA;
            }, true, true);
    }
}

TEST_F(JitParity64Test, RepeatedPaddqXmm8ForwardsV0AndMatchesInterpreter) {
    // Intel SDM Vol. 2 PADDQ: two packed qwords wrap modulo 2^64 and flags are
    // unaffected.  Repeat the high-register form that the x64 perf catalog
    // measures so the architectural result and the eliminated state reload
    // are pinned together.
    static const u8 code[] = {
        0x66, 0x45, 0x0F, 0xD4, 0xC1,  // paddq xmm8,xmm9
        0x66, 0x45, 0x0F, 0xD4, 0xC1,
        0x66, 0x45, 0x0F, 0xD4, 0xC1,
        0x66, 0x45, 0x0F, 0xD4, 0xC1,
    };
    auto seed = [](CpuState& cpu) {
        cpu.flags.rflags = 0xA43;
        cpu.sse.xmm[8].u64_[0] = 0xFFFFFFFFFFFFFFFCull;
        cpu.sse.xmm[8].u64_[1] = 0x8000000000000000ull;
        cpu.sse.xmm[9].u64_[0] = 3;
        cpu.sse.xmm[9].u64_[1] = 0x4000000000000001ull;
    };
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    check_parity(code, sizeof(code), seed, /*check_flags=*/true,
                 /*check_xmm=*/true, /*require_jit=*/true);

    BasicBlock block = build_block(code, sizeof(code));
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    const auto* words = static_cast<const u32*>(compiled->code);
    const usize count = compiled->code_size / sizeof(u32);
    // The contract is "only the FIRST PADDQ reloads XMM8"; which vector
    // register holds the forwarded value is the tier's own choice. Tier 0
    // keeps it in V0, Tier 1 hands out registers in the order its frontend
    // reads operands -- so the Rt field is masked out and the SLOT is what is
    // counted. Asserting the register number pinned a mechanism and failed the
    // moment a second tier learned the same optimization.
    const u32 xmm8_reload = arm64::ldr_q_uoff(
        0, REG_CPU_STATE, xmm_u64_off(8, 0) >> 4);
    const auto reloads = std::count_if(
        words, words + count,
        [&](u32 w) { return (w & ~0x1Fu) == (xmm8_reload & ~0x1Fu); });
    EXPECT_EQ(reloads, 1)
        << "only the first PADDQ may reload XMM8; later results stay in a "
           "vector register";
}
