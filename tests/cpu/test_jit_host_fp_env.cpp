// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT execution tests — host FP environment (MXCSR FTZ/DAZ) across JIT entry

#include "jit_exec_test_fixture.hpp"

// MXCSR.FTZ / DAZ (Intel SDM Vol. 1 10.2.3.3-4) in both engines. Every
// expected word was measured on an Intel i9-12900KF (Windows) with the same
// instructions: FTZ flushes tiny RESULTS only -- a denormal INPUT still takes
// part (den * 2^20 stays 0x0A000000) and CVTSS2SD widens it exactly -- and
// (1 - 2^-24) * FLT_MIN flushes although it rounds to FLT_MIN; DAZ flushes
// denormal INPUTS only, so a tiny result survives. FTZ without DAZ is the
// mode Unreal sets; the host implements it with FPCR.FZ|AH (host_fenv.cpp).
TEST(HostFpEnvironment, SseCertificateTracksOwnerInvalidationAndThread) {
    const u64* key = math::installed_sse_environment_key();
    math::forget_host_rounding();
    EXPECT_GT(*key, std::numeric_limits<u32>::max());
    math::apply_sse_rounding(0x1F80u);
    EXPECT_EQ(*key, 0x1F80u);
    math::apply_x87_rounding(0x037Fu);
    EXPECT_GT(*key, std::numeric_limits<u32>::max());
    math::apply_sse_rounding(0x9FC0u);
    EXPECT_EQ(*key, 0x9FC0u);
    std::thread other([&] {
        const u64* other_key = math::installed_sse_environment_key();
        EXPECT_NE(other_key, key);
        EXPECT_GT(*other_key, std::numeric_limits<u32>::max());
        math::apply_sse_rounding(0x3F80u);
        EXPECT_EQ(*other_key, 0x3F80u);
    });
    other.join();
    EXPECT_EQ(*key, 0x9FC0u);
    math::forget_host_rounding();
    EXPECT_GT(*key, std::numeric_limits<u32>::max());
    math::apply_sse_rounding(0x1F80u);
}

TEST_F(JitExecTest, SseEnvironmentGuardPreservesRoundingFlushAndFlagsBothModes) {
    // Intel SDM Vol. 1 10.2.3: all RC/FTZ/DAZ combinations, independent
    // x87 ownership, and the same arithmetic contract in x86 and x64.
    // Flush expectations also match X64MxcsrFlushModesMatchX86Silicon below.
    const u8 code[] = {
        0xF3, 0x0F, 0x59, 0xC1, // mulss xmm0,xmm1: denormal * 2^20
        0xF3, 0x0F, 0x59, 0xD3, // mulss xmm2,xmm3: FLT_MIN * 0.5
        0xF3, 0x0F, 0x2D, 0xC4, // cvtss2si eax,xmm4: +1.5
        0xF3, 0x0F, 0x2D, 0xD5, // cvtss2si edx,xmm5: -1.5
    };
    constexpr u32 modes[] = {0x1F80u, 0x3F80u, 0x5F80u, 0x7F80u};
    constexpr u32 positive[] = {2u, 1u, 2u, 1u};
    constexpr i32 negative[] = {-2, -2, -1, -1};
    for (bool wide : {false, true}) {
        auto block = wide ? build_block64(code, sizeof(code))
                          : build_block(code, sizeof(code));
        auto* compiled_ptr = m_jit->compile(block);
        ASSERT_NE(compiled_ptr, nullptr);
        const CompiledBlock compiled = *compiled_ptr;
        for (u32 rc = 0; rc < 4; ++rc) {
            for (u32 flush : {0u, 0x8000u, 0x40u, 0x8040u}) {
                const u32 mxcsr = modes[rc] | flush;
                for (unsigned entry = 0; entry < 3; ++entry) {
                    SCOPED_TRACE(wide);
                    SCOPED_TRACE(mxcsr);
                    SCOPED_TRACE(entry);
                    if (entry == 0) math::apply_x87_rounding(0x0F7Fu);
                    if (entry == 2) math::forget_host_rounding();
                    wide ? m_cpu->init_long_mode() : m_cpu->init_protected_mode();
                    m_cpu->rip = 0x1000;
                    m_cpu->gpr.rsp = 0x10000;
                    m_cpu->flags.rflags = 0x8D7u;
                    m_cpu->sse.mxcsr = mxcsr;
                    m_cpu->sse.xmm[0].u32_[0] = 0x00400000u;
                    m_cpu->sse.xmm[1].u32_[0] = 0x49800000u;
                    m_cpu->sse.xmm[2].u32_[0] = 0x00800000u;
                    m_cpu->sse.xmm[3].u32_[0] = 0x3F000000u;
                    m_cpu->sse.xmm[4].u32_[0] = 0x3FC00000u;
                    m_cpu->sse.xmm[5].u32_[0] = 0xBFC00000u;
                    auto run = compiled;
                    ASSERT_EQ(test::execute_like_dispatcher(*m_jit, run, *m_cpu), JitExecResult::Continue);
                    EXPECT_FALSE(m_cpu->pending_raise.active);
                    EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0],
                              (flush & 0x40u) ? 0u : 0x0A000000u);
                    EXPECT_EQ(m_cpu->sse.xmm[2].u32_[0],
                              (flush & 0x8000u) ? 0u : 0x00400000u);
                    EXPECT_EQ(m_cpu->gpr.get32(EAX), positive[rc]);
                    EXPECT_EQ(m_cpu->gpr.get32(EDX), static_cast<u32>(negative[rc]));
                    m_cpu->materialize_lazy_flags();
                    EXPECT_EQ(m_cpu->flags.rflags, 0x8D7u);
                    EXPECT_EQ(m_cpu->gpr.rsp, 0x10000u);
                }
            }
        }
    }
    math::apply_sse_rounding(0x1F80u);
}

TEST_F(JitExecTest, SseEnvironmentEntryPreservesLinkedCompareBothModes) {
    // MULSS and JMP preserve EFLAGS (Intel SDM Vol. 2). The successor's JNZ
    // must consume the predecessor's CMP even when SSE environment entry
    // needs its cold helper rather than the certificate fast path.
    JitLinkGuard link_guard;
    for (bool wide : {false, true}) {
        BlockCache cache;
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_jit->set_block_cache(&cache);
        const auto publish = [&](GuestAddr address, const std::vector<u8>& code) {
            BasicBlock block = wide
                ? build_block64_at(address, code.data(), code.size())
                : build_block(code.data(), code.size());
            if (!wide) {
                block.start_addr = address;
                EXPECT_TRUE(m_memory->write_bytes(address, code.data(), code.size()));
            }
            auto* cached = cache.insert(std::move(block));
            EXPECT_NE(cached, nullptr);
            auto* compiled = m_jit->compile(*cached);
            EXPECT_NE(compiled, nullptr);
            if (compiled) publish_compiled(*cached, *compiled);
            return compiled ? *compiled : CompiledBlock{};
        };
        publish(0x3000, {0xBA,0x33,0,0,0}); // taken: mov edx,0x33
        publish(0x200A, {0xBA,0x22,0,0,0}); // fallthrough: mov edx,0x22
        publish(0x1009, {0xBA,0x22,0,0,0}); // first comparison was equal
        publish(0x2000, {0xF3,0x0F,0x59,0xC1,0x0F,0x85,0xF6,0x0F,0,0});
        auto entry = publish(0x1000, {0x83,0xF8,1,0x0F,0x85,0xF7,0x0F,0,0});
        ASSERT_NE(entry.code, nullptr);
        for (unsigned environment : {0u, 1u, 2u}) {
            for (u32 eax : {0u, 1u}) {
                SCOPED_TRACE(wide);
                SCOPED_TRACE(environment);
                SCOPED_TRACE(eax);
                math::apply_sse_rounding(0x1F80u);
                if (environment == 0) math::forget_host_rounding();
                // Same rounding/flush state but a different full certificate:
                // this forces the guarded helper to take its own memo hits.
                if (environment == 2) math::apply_sse_rounding(0x1F81u);
                wide ? m_cpu->init_long_mode() : m_cpu->init_protected_mode();
                m_cpu->rip = 0x1000;
                m_cpu->gpr.rax = eax;
                m_cpu->gpr.rsp = 0x10000;
                m_cpu->sse.xmm[0].u32_[0] = 0x3FC00000u;
                m_cpu->sse.xmm[1].u32_[0] = 0x3F800000u;
                ASSERT_EQ(test::execute_like_dispatcher(*m_jit, entry, *m_cpu), JitExecResult::Continue);
                EXPECT_EQ(m_cpu->gpr.get32(EDX), eax == 1 ? 0x22u : 0x33u);
                EXPECT_EQ(m_cpu->sse.xmm[0].u32_[0], 0x3FC00000u);
            }
        }
        m_jit->set_block_cache(nullptr);
    }
}

TEST_F(JitExecTest, X64MxcsrFlushModesMatchX86Silicon) {
    const u8 code[] = {
        0xF3, 0x0F, 0x59, 0xC1,        // mulss    xmm0, xmm1
        0xF3, 0x0F, 0x59, 0xD3,        // mulss    xmm2, xmm3
        0xF3, 0x0F, 0x59, 0xE5,        // mulss    xmm4, xmm5
        0xF3, 0x0F, 0x5A, 0xF7,        // cvtss2sd xmm6, xmm7
        0xF2, 0x45, 0x0F, 0x5A, 0xC1,  // cvtsd2ss xmm8, xmm9
    };
    constexpr u32 kDen = 0x00400000u;        // 2^-127, a single denormal
    constexpr u32 kFltMin = 0x00800000u;
    struct Expect {
        u32 mxcsr;
        u32 den_times_one;          // xmm0: den * 1.0
        u32 just_below_flt_min;     // xmm2: (1 - 2^-24) * FLT_MIN
        u32 den_times_2_20;         // xmm4: den * 2^20
        u64 widened_den;            // xmm6: cvtss2sd(den)
        u32 narrowed_tiny;          // xmm8: cvtsd2ss(2^-130)
    };
    constexpr Expect modes[] = {
        {0x1F80u, kDen, kFltMin, 0x0A000000u, 0x3800000000000000ull,
         0x00080000u},                                          // neither
        {0x9F80u, 0u, 0u, 0x0A000000u, 0x3800000000000000ull, 0u},  // FTZ
        {0x9FC0u, 0u, 0u, 0u, 0ull, 0u},                        // FTZ | DAZ
        {0x1FC0u, 0u, kFltMin, 0u, 0ull, 0x00080000u},          // DAZ
    };
    const auto seed = [&](CpuState& cpu, u32 mxcsr) {
        cpu.init_long_mode();
        cpu.gpr.rsp = 0x10000;
        cpu.rip = 0x1000;
        cpu.sse.mxcsr = mxcsr;
        cpu.sse.xmm[0].u32_[0] = kDen;
        cpu.sse.xmm[1].u32_[0] = 0x3F800000u;      // 1.0
        cpu.sse.xmm[2].u32_[0] = 0x3F7FFFFFu;      // 1 - 2^-24
        cpu.sse.xmm[3].u32_[0] = kFltMin;
        cpu.sse.xmm[4].u32_[0] = kDen;
        cpu.sse.xmm[5].u32_[0] = 0x49800000u;      // 2^20
        cpu.sse.xmm[7].u32_[0] = kDen;
        cpu.sse.xmm[9].u64_[0] = 0x37D0000000000000ull;  // 2^-130
    };
    const auto check = [](const CpuState& cpu, const Expect& e,
                          const char* engine) {
        EXPECT_EQ(cpu.sse.xmm[0].u32_[0], e.den_times_one)
            << engine << " mxcsr " << std::hex << e.mxcsr;
        EXPECT_EQ(cpu.sse.xmm[2].u32_[0], e.just_below_flt_min)
            << engine << " mxcsr " << std::hex << e.mxcsr;
        EXPECT_EQ(cpu.sse.xmm[4].u32_[0], e.den_times_2_20)
            << engine << " mxcsr " << std::hex << e.mxcsr;
        EXPECT_EQ(cpu.sse.xmm[6].u64_[0], e.widened_den)
            << engine << " mxcsr " << std::hex << e.mxcsr;
        EXPECT_EQ(cpu.sse.xmm[8].u32_[0], e.narrowed_tiny)
            << engine << " mxcsr " << std::hex << e.mxcsr;
    };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    for (const Expect& e : modes) {
        CpuState reference;
        seed(reference, e.mxcsr);
        Interpreter interpreter(reference, *m_memory);
        (void)interpreter.run(5);
        check(reference, e, "interpreter");

        seed(*m_cpu, e.mxcsr);
        EXPECT_EQ(jit_run64(code, sizeof(code)), JitExecResult::Continue);
        check(*m_cpu, e, "jit");
    }
    // Leave this thread's host FP environment at the default for later tests.
    vitreo::math::apply_sse_rounding(0x1F80u);
}
