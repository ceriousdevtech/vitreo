// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// PCLMULQDQ on ARMv8 PMULL JIT contract tests (WS3)
//
// Register and memory forms in every memory configuration, checked against a
// test-local shift-xor carry-less multiply (Intel SDM Vol. 2 PCLMULQDQ
// Operation) and GF(2) identities, plus interpreter parity and the absence of
// the jit_sse_op dispatch.

#include "jit_crypto_test_harness.hpp"

using namespace ws3test;

namespace {

// 64x64 -> 128 carry-less product, bit by bit.
void clmul64(u64 a, u64 b, u64& lo, u64& hi) {
    lo = hi = 0;
    for (int i = 0; i < 64; ++i) {
        if ((b >> i) & 1) {
            lo ^= a << i;
            if (i != 0) hi ^= a >> (64 - i);
        }
    }
}

void expect_clmul(const CpuState& out, const XmmReg& s1, const XmmReg& s2,
                  u8 imm) {
    u64 lo = 0, hi = 0;
    clmul64(s1.u64_[imm & 1], s2.u64_[(imm >> 4) & 1], lo, hi);
    EXPECT_EQ(out.sse.xmm[0].u64_[0], lo);
    EXPECT_EQ(out.sse.xmm[0].u64_[1], hi);
}

constexpr std::array<u8, 6> kImms{0x00, 0x01, 0x10, 0x11, 0xEE, 0xFF};

std::vector<u8> mem_code(u8 imm) { return {0x66, 0x0F, 0x3A, 0x44, 0x02, imm}; }
std::vector<u8> reg_code(u8 imm) { return {0x66, 0x0F, 0x3A, 0x44, 0xC1, imm}; }

TEST(JitPclmulNativeTest, HostAdvertisesFeatPmull) {
    EXPECT_TRUE(host_supports_pmull());
}

TEST(JitPclmulNativeTest, MemoryFormAllImmAllConfigs) {
    for (MemCfg cfg : kAllCfgs) {
        CryptoRig rig;
        if (!rig.init(cfg)) {
            ADD_FAILURE() << "linear reservation unavailable: " << cfg_name(cfg);
            continue;
        }
        for (u8 imm : kImms) {
            u64 seed = 0x9C1AA1ULL + imm;
            for (int v = 0; v < 32; ++v) {
                SCOPED_TRACE(::testing::Message() << cfg_name(cfg) << " imm="
                             << int(imm) << " v=" << v);
                CpuState init = rig.base();
                set_xmm(init, 0, next_rand(seed), next_rand(seed));
                XmmReg m{};
                m.u64_[0] = next_rand(seed);
                m.u64_[1] = next_rand(seed);
                put_m128(rig, kData, m.u64_[0], m.u64_[1]);
                const XmmReg s1 = init.sse.xmm[0];
                const JitRun r = expect_parity(rig, mem_code(imm), init);
                expect_clmul(r.cpu, s1, m, imm);
                EXPECT_FALSE(has_sse_op_call(r.words));
            }
        }
    }
}

TEST(JitPclmulNativeTest, RegisterFormAllImm) {
    for (MemCfg cfg : {MemCfg::NonLinear32, MemCfg::Region64}) {
        CryptoRig rig;
        ASSERT_TRUE(rig.init(cfg));
        for (u8 imm : kImms) {
            SCOPED_TRACE(::testing::Message() << cfg_name(cfg) << " imm=" << int(imm));
            const CpuState init = rig.base();
            const JitRun r = expect_parity(rig, reg_code(imm), init);
            expect_clmul(r.cpu, init.sse.xmm[0], init.sse.xmm[1], imm);
            EXPECT_FALSE(has_sse_op_call(r.words));
        }
    }
}

TEST(JitPclmulNativeTest, KnownProductIdentity) {
    CryptoRig rig;
    ASSERT_TRUE(rig.init(MemCfg::NonLinear32));
    struct Case { u64 a, b, lo, hi; };
    // (sum x^i)^2 = sum x^(2i) over GF(2); 1*1 = 1; x^63 * x = x^64.
    const Case cases[] = {
        {~u64{0}, ~u64{0}, 0x5555555555555555ULL, 0x5555555555555555ULL},
        {1, 1, 1, 0},
        {0x8000000000000000ULL, 2, 0, 1},
    };
    for (const Case& c : cases) {
        CpuState init = rig.base();
        set_xmm(init, 0, c.a, 0x1234);
        put_m128(rig, kData, c.b, 0x5678);
        const JitRun r = expect_parity(rig, mem_code(0x00), init);
        EXPECT_EQ(r.cpu.sse.xmm[0].u64_[0], c.lo);
        EXPECT_EQ(r.cpu.sse.xmm[0].u64_[1], c.hi);
    }
}

TEST(JitPclmulNativeTest, AliasDstEqualsSrc) {
    for (MemCfg cfg : {MemCfg::NonLinear32, MemCfg::Region64}) {
        CryptoRig rig;
        ASSERT_TRUE(rig.init(cfg));
        const CpuState init = rig.base();
        const JitRun r = expect_parity(
            rig, {0x66, 0x0F, 0x3A, 0x44, 0xC0, 0x11}, init);
        expect_clmul(r.cpu, init.sse.xmm[0], init.sse.xmm[0], 0x11);
    }
}

TEST(JitPclmulNativeTest, MisalignedRaisesGp) {
    for (MemCfg cfg : {MemCfg::NonLinear32, MemCfg::Region64}) {
        CryptoRig rig;
        ASSERT_TRUE(rig.init(cfg));
        CpuState init = rig.base();
        init.gpr.set64(kRdx, kData + 8);
        CpuState interpreted = init;
        EXPECT_EQ(rig.interp(mem_code(0x11), interpreted), ExecResult::Exception);
        expect_fault_untouched(rig.jit(mem_code(0x11), init), init);
    }
}

TEST(JitPclmulNativeTest, FaultLeavesDestination) {
    for (MemCfg cfg : {MemCfg::NonLinear32, MemCfg::Region64}) {
        CryptoRig rig;
        ASSERT_TRUE(rig.init(cfg));
        CpuState init = rig.base();
        init.gpr.set64(kRdx, kUnmapped);
        expect_fault_untouched(rig.jit(mem_code(0x01), init), init);
    }
}

TEST(JitPclmulNativeTest, Ws3KillSwitchRestoresMemoryHelper) {
    ScopedEnv off("VITREO_WS3", "0");
    for (MemCfg cfg : {MemCfg::NonLinear32, MemCfg::Region64}) {
        CryptoRig rig;
        ASSERT_TRUE(rig.init(cfg));
        CpuState init = rig.base();
        put_m128(rig, kData, 0xDEADBEEFULL, 0xFEEDFACEULL);
        const JitRun r = expect_parity(rig, mem_code(0x10), init);
        EXPECT_TRUE(has_sse_op_call(r.words));
    }
}

} // namespace
