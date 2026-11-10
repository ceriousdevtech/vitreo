// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Direct ARMv8 FEAT_SHA1 JIT contract tests (WS3)
//
// SHA1RNDS4 / SHA1NEXTE / SHA1MSG1 / SHA1MSG2 in every block width and memory
// configuration are emitted without the jit_sse_op helper. Expected values
// come from a test-local reference written from the Intel SDM Vol. 2
// "Operation" sections and from the FIPS 180-4 "abc" digest, not from the
// interpreter.

#include "jit_crypto_test_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

using namespace ws3test;

namespace {

u32 rol(u32 x, int n) { return (x << n) | (x >> (32 - n)); }

// x86 keeps the first word of a quad in bits 127:96 (u32_[3]).
struct Quad { u32 w[4]; };  // w[0] = [127:96] ... w[3] = [31:0]
Quad quad_of(const XmmReg& x) {
    return {{x.u32_[3], x.u32_[2], x.u32_[1], x.u32_[0]}};
}
void put_quad(XmmReg& x, const Quad& q) {
    x.u32_[3] = q.w[0]; x.u32_[2] = q.w[1]; x.u32_[1] = q.w[2]; x.u32_[0] = q.w[3];
}

// SDM SHA1RNDS4.
Quad ref_rnds4(const Quad& s1, const Quad& s2, u8 imm) {
    static constexpr u32 kK[4] = {0x5A827999u, 0x6ED9EBA1u, 0x8F1BBCDCu,
                                  0xCA62C1D6u};
    auto f = [imm](u32 b, u32 c, u32 d) -> u32 {
        switch (imm & 3) {
        case 0: return (b & c) ^ (~b & d);
        case 2: return (b & c) ^ (b & d) ^ (c & d);
        default: return b ^ c ^ d;
        }
    };
    u32 a = s1.w[0], b = s1.w[1], c = s1.w[2], d = s1.w[3], e = 0;
    for (int i = 0; i < 4; ++i) {
        const u32 na = f(b, c, d) + rol(a, 5) + s2.w[i] + e + kK[imm & 3];
        e = d; d = c; c = rol(b, 30); b = a; a = na;
    }
    return {{a, b, c, d}};
}
// SDM SHA1NEXTE.
Quad ref_nexte(const Quad& s1, const Quad& s2) {
    return {{s2.w[0] + rol(s1.w[0], 30), s2.w[1], s2.w[2], s2.w[3]}};
}
// SDM SHA1MSG1: W0..W3 = SRC1 [127:96]..[31:0], W4/W5 = SRC2[127:96]/[95:64].
Quad ref_msg1(const Quad& s1, const Quad& s2) {
    return {{s1.w[2] ^ s1.w[0], s1.w[3] ^ s1.w[1], s2.w[0] ^ s1.w[2],
             s2.w[1] ^ s1.w[3]}};
}
// SDM SHA1MSG2: W13..W15 = SRC2[95:64]..[31:0].
Quad ref_msg2(const Quad& s1, const Quad& s2) {
    const u32 w16 = rol(s1.w[0] ^ s2.w[1], 1);
    const u32 w17 = rol(s1.w[1] ^ s2.w[2], 1);
    const u32 w18 = rol(s1.w[2] ^ s2.w[3], 1);
    const u32 w19 = rol(s1.w[3] ^ w16, 1);
    return {{w16, w17, w18, w19}};
}

struct Form {
    const char* name;
    std::vector<u8> reg;   // dst xmm0, src xmm1
    std::vector<u8> mem;   // dst xmm0, src [rdx]
    std::vector<u8> self;  // xmm0, xmm0
    u8 imm;
    bool rnds;
};

std::vector<Form> forms() {
    std::vector<Form> f;
    for (u8 imm : {u8{0}, u8{1}, u8{2}, u8{3}, u8{0xFC}}) {
        f.push_back({"rnds4", {0x0F, 0x3A, 0xCC, 0xC1, imm},
                     {0x0F, 0x3A, 0xCC, 0x02, imm},
                     {0x0F, 0x3A, 0xCC, 0xC0, imm}, imm, true});
    }
    f.push_back({"nexte", {0x0F, 0x38, 0xC8, 0xC1}, {0x0F, 0x38, 0xC8, 0x02},
                 {0x0F, 0x38, 0xC8, 0xC0}, 0, false});
    f.push_back({"msg1", {0x0F, 0x38, 0xC9, 0xC1}, {0x0F, 0x38, 0xC9, 0x02},
                 {0x0F, 0x38, 0xC9, 0xC0}, 0, false});
    f.push_back({"msg2", {0x0F, 0x38, 0xCA, 0xC1}, {0x0F, 0x38, 0xCA, 0x02},
                 {0x0F, 0x38, 0xCA, 0xC0}, 0, false});
    return f;
}

Quad reference(const Form& f, const Quad& s1, const Quad& s2) {
    if (f.rnds) return ref_rnds4(s1, s2, f.imm);
    if (f.reg[2] == 0xC8) return ref_nexte(s1, s2);
    if (f.reg[2] == 0xC9) return ref_msg1(s1, s2);
    return ref_msg2(s1, s2);
}

void expect_quad(const XmmReg& x, const Quad& q) {
    const Quad got = quad_of(x);
    for (int i = 0; i < 4; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(got.w[i], q.w[i]);
    }
}

TEST(JitSha1NativeTest, HostAdvertisesFeatSha1) {
    EXPECT_TRUE(host_supports_sha1());
}

TEST(JitSha1NativeTest, RegisterFormsMatchReferenceWithoutHelperCall) {
    for (MemCfg cfg : {MemCfg::NonLinear32, MemCfg::Region64}) {
        CryptoRig rig;
        ASSERT_TRUE(rig.init(cfg));
        for (const Form& f : forms()) {
            u64 seed = 0x5EED5A1ULL;
            // 32 seeded vectors plus the asymmetric MSG2 lane vector of
            // JitParityTest.Sha1msg2PreservesIntelLaneOrder.
            for (int v = 0; v < 33; ++v) {
                SCOPED_TRACE(::testing::Message() << cfg_name(cfg) << " "
                             << f.name << " imm=" << int(f.imm) << " v=" << v);
                CpuState init = rig.base();
                if (v == 32) {
                    const u32 a[4] = {0x01234567u, 0x89ABCDEFu, 0xFEDCBA98u,
                                      0x76543210u};
                    const u32 b[4] = {0x0F1E2D3Cu, 0x4B5A6978u, 0x8796A5B4u,
                                      0xC3D2E1F0u};
                    std::memcpy(init.sse.xmm[0].u32_, a, 16);
                    std::memcpy(init.sse.xmm[1].u32_, b, 16);
                } else {
                    set_xmm(init, 0, next_rand(seed), next_rand(seed));
                    set_xmm(init, 1, next_rand(seed), next_rand(seed));
                }
                const Quad want = reference(f, quad_of(init.sse.xmm[0]),
                                            quad_of(init.sse.xmm[1]));
                const JitRun r = expect_parity(rig, f.reg, init);
                expect_quad(r.cpu.sse.xmm[0], want);
                EXPECT_FALSE(has_sse_op_call(r.words));
            }
        }
    }
}

TEST(JitSha1NativeTest, MemoryFormsAllConfigs) {
    for (MemCfg cfg : kAllCfgs) {
        CryptoRig rig;
        if (!rig.init(cfg)) {
            ADD_FAILURE() << "linear reservation unavailable: " << cfg_name(cfg);
            continue;
        }
        for (const Form& f : forms()) {
            u64 seed = 0xA11C0F1ULL;
            for (int v = 0; v < 4; ++v) {
                SCOPED_TRACE(::testing::Message() << cfg_name(cfg) << " "
                             << f.name << " imm=" << int(f.imm) << " v=" << v);
                CpuState init = rig.base();
                set_xmm(init, 0, next_rand(seed), next_rand(seed));
                XmmReg m{};
                m.u64_[0] = next_rand(seed);
                m.u64_[1] = next_rand(seed);
                put_m128(rig, kData, m.u64_[0], m.u64_[1]);
                const Quad want = reference(f, quad_of(init.sse.xmm[0]),
                                            quad_of(m));
                const JitRun r = expect_parity(rig, f.mem, init);
                expect_quad(r.cpu.sse.xmm[0], want);
                EXPECT_FALSE(has_sse_op_call(r.words));
            }
        }
    }
}

TEST(JitSha1NativeTest, AliasDstEqualsSrc) {
    for (MemCfg cfg : {MemCfg::NonLinear32, MemCfg::Region64}) {
        CryptoRig rig;
        ASSERT_TRUE(rig.init(cfg));
        for (const Form& f : forms()) {
            SCOPED_TRACE(::testing::Message() << cfg_name(cfg) << " " << f.name);
            const CpuState init = rig.base();
            const Quad q = quad_of(init.sse.xmm[0]);
            const JitRun r = expect_parity(rig, f.self, init);
            expect_quad(r.cpu.sse.xmm[0], reference(f, q, q));
            EXPECT_FALSE(has_sse_op_call(r.words));
        }
    }
}

TEST(JitSha1NativeTest, MisalignedRaisesGp) {
    for (MemCfg cfg : {MemCfg::NonLinear32, MemCfg::Region64}) {
        CryptoRig rig;
        ASSERT_TRUE(rig.init(cfg));
        for (const Form& f : forms()) {
            SCOPED_TRACE(::testing::Message() << cfg_name(cfg) << " " << f.name);
            CpuState init = rig.base();
            init.gpr.set64(kRdx, kData + 8);
            CpuState interpreted = init;
            EXPECT_EQ(rig.interp(f.mem, interpreted), ExecResult::Exception);
            expect_fault_untouched(rig.jit(f.mem, init), init);
        }
    }
}

TEST(JitSha1NativeTest, FaultLeavesDestination) {
    for (MemCfg cfg : {MemCfg::NonLinear32, MemCfg::Region64}) {
        CryptoRig rig;
        ASSERT_TRUE(rig.init(cfg));
        for (const Form& f : forms()) {
            SCOPED_TRACE(::testing::Message() << cfg_name(cfg) << " " << f.name);
            CpuState init = rig.base();
            init.gpr.set64(kRdx, kUnmapped);
            expect_fault_untouched(rig.jit(f.mem, init), init);
        }
    }
}

TEST(JitSha1NativeTest, Ws3KillSwitchRestoresHelperIn32Bit) {
    ScopedEnv off("VITREO_WS3", "0");
    CryptoRig rig;
    ASSERT_TRUE(rig.init(MemCfg::NonLinear32));
    for (const Form& f : forms()) {
        if (f.rnds) continue;  // SHA1RNDS4 register was direct before WS3
        SCOPED_TRACE(f.name);
        const JitRun r = expect_parity(rig, f.reg, rig.base());
        EXPECT_TRUE(has_sse_op_call(r.words));
    }
}

// ---- FIPS 180-4 Appendix A.1 "abc", entirely through JIT SHA-1 opcodes ----

class Sha1Runner {
public:
    Sha1Runner() { m_ok = m_rig.init(MemCfg::NonLinear32); }
    bool ok() const { return m_ok; }
    bool saw_helper() const { return m_saw_helper; }

    XmmReg run(const std::vector<u8>& code, const XmmReg& x0, const XmmReg& x1,
               const XmmReg& x2 = {}, const XmmReg& x3 = {}) {
        CpuState cpu = m_rig.base();
        cpu.sse.xmm[0] = x0; cpu.sse.xmm[1] = x1;
        cpu.sse.xmm[2] = x2; cpu.sse.xmm[3] = x3;
        const JitRun r = m_rig.jit(code, cpu);
        EXPECT_TRUE(r.compiled);
        EXPECT_EQ(r.result, JitExecResult::Continue);
        m_saw_helper |= has_sse_op_call(r.words);
        return r.cpu.sse.xmm[0];
    }

private:
    CryptoRig m_rig;
    bool m_ok{false};
    bool m_saw_helper{false};
};

TEST(JitSha1NativeTest, Fips180Sha1AbcDigestThroughJit) {
    Sha1Runner run;
    ASSERT_TRUE(run.ok());

    // One padded block for "abc" (FIPS 180-4 §5.1.1), big-endian words.
    u32 w[80] = {0x61626380u};
    w[15] = 24;
    for (int t = 16; t < 80; ++t) {
        w[t] = rol(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    }

    // Message quads, schedule W16..W79 produced by JIT SHA1MSG1/PXOR/MSG2.
    XmmReg msg[20];
    for (int i = 0; i < 4; ++i) {
        put_quad(msg[i], {{w[4 * i], w[4 * i + 1], w[4 * i + 2], w[4 * i + 3]}});
    }
    // SHA1MSG1 xmm0,xmm1 ; PXOR xmm0,xmm2 ; SHA1MSG2 xmm0,xmm3
    const std::vector<u8> sched{0x0F, 0x38, 0xC9, 0xC1, 0x66, 0x0F, 0xEF, 0xC2,
                                0x0F, 0x38, 0xCA, 0xC3};
    for (int i = 4; i < 20; ++i) {
        msg[i] = run.run(sched, msg[i - 4], msg[i - 3], msg[i - 2], msg[i - 1]);
        SCOPED_TRACE(i);
        expect_quad(msg[i],
                    {{w[4 * i], w[4 * i + 1], w[4 * i + 2], w[4 * i + 3]}});
    }

    // FIPS 180-4 §5.3.1 initial hash value.
    const u32 h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                      0xC3D2E1F0u};
    XmmReg abcd{};
    put_quad(abcd, {{h[0], h[1], h[2], h[3]}});
    // Round group 0: E0 = H4 + W0 in the top lane, W1..W3 below it.
    XmmReg e{};
    put_quad(e, {{w[0] + h[4], w[1], w[2], w[3]}});
    const std::vector<u8> nexte{0x0F, 0x38, 0xC8, 0xC1};  // SHA1NEXTE xmm0,xmm1
    XmmReg saved = abcd;
    for (int i = 0; i < 20; ++i) {
        if (i > 0) e = run.run(nexte, saved, msg[i]);
        saved = abcd;
        const u8 imm = static_cast<u8>(i / 5);
        abcd = run.run({0x0F, 0x3A, 0xCC, 0xC1, imm}, abcd, e);  // SHA1RNDS4
    }
    // E after round 79 = ROL30(A before the last group): SHA1NEXTE with 0.
    const XmmReg e_final = run.run(nexte, saved, XmmReg{});

    const Quad q = quad_of(abcd);
    const u32 digest[5] = {q.w[0] + h[0], q.w[1] + h[1], q.w[2] + h[2],
                           q.w[3] + h[3], e_final.u32_[3] + h[4]};
    const u32 expected[5] = {0xa9993e36u, 0x4706816au, 0xba3e2571u,
                             0x7850c26cu, 0x9cd0d89du};
    for (int i = 0; i < 5; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(digest[i], expected[i]);
    }
    EXPECT_FALSE(run.saw_helper());
}

// ===========================================================================
// WS3 gate: direct vs helper, per-instruction ns. Skipped unless
// VITREO_WS3_BENCH=1 so a normal ctest never times anything. Arm A is the
// default build, arm B the same block compiled with VITREO_WS3=0 (delegation
// allowed, so a form the pre-WS3 JIT could not lower runs as it used to).
// Median of 5 interleaved reps of 20,000 executions of 32 copies.
// ===========================================================================

struct BenchForm {
    const char* name;
    MemCfg cfg;
    std::vector<u8> insn;
    bool must_win;
};

double bench_ns_per_insn(CryptoRig& rig, const std::vector<u8>& code,
                         bool ws3_on, u32 copies) {
    std::optional<ScopedEnv> off;
    if (!ws3_on) off.emplace("VITREO_WS3", "0");
    BasicBlock block = rig.decode(code);
    EXPECT_TRUE(rig.mem().write_bytes(kCode, code.data(), code.size()));
    JitCompiler jitc(rig.mem());
    CompiledBlock* compiled = jitc.compile(block);
    EXPECT_NE(compiled, nullptr);
    if (!compiled) return 0;
    const CpuState init = rig.base();
    CpuState cpu = init;
    constexpr int kIters = 20000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i) {
        cpu.rip = kCode;
        jitc.execute(*compiled, cpu);
    }
    const auto t1 = std::chrono::steady_clock::now();
    EXPECT_FALSE(cpu.pending_raise.active);
    return std::chrono::duration<double, std::nano>(t1 - t0).count() /
           (double(kIters) * copies);
}

TEST(JitCryptoBench, DirectVsHelperPerInstruction) {
    const char* on = std::getenv("VITREO_WS3_BENCH");
    if (on == nullptr || std::strcmp(on, "1") != 0) {
        GTEST_SKIP() << "set VITREO_WS3_BENCH=1 to run the WS3 benchmark";
    }
    const std::vector<BenchForm> forms{
        {"F1 SHA256RNDS2 reg (64-bit)", MemCfg::Region64, {0x0F, 0x38, 0xCB, 0xCA}, true},
        {"F2 SHA256RNDS2 mem (linear64)", MemCfg::Linear64, {0x0F, 0x38, 0xCB, 0x0A}, true},
        {"F3 SHA256MSG1 mem (linear64)", MemCfg::Linear64, {0x0F, 0x38, 0xCC, 0x0A}, true},
        {"F4 SHA1NEXTE reg (32-bit)", MemCfg::NonLinear32, {0x0F, 0x38, 0xC8, 0xC1}, true},
        {"F5 SHA1RNDS4 mem (32-bit linear)", MemCfg::Linear32, {0x0F, 0x3A, 0xCC, 0x02, 0x01}, true},
        {"F6 PCLMULQDQ mem 0x11 (linear64)", MemCfg::Linear64, {0x66, 0x0F, 0x3A, 0x44, 0x02, 0x11}, true},
        {"F7 PCLMULQDQ mem (x64 region)", MemCfg::Region64, {0x66, 0x0F, 0x3A, 0x44, 0x02, 0x11}, true},
        {"F8 AESKEYGENASSIST reg", MemCfg::Region64, {0x66, 0x0F, 0x3A, 0xDF, 0xC1, 0x1B}, true},
        {"F9 AESKEYGENASSIST mem (linear64)", MemCfg::Linear64, {0x66, 0x0F, 0x3A, 0xDF, 0x02, 0x1B}, true},
        {"F10 vaesenc x1,x2,x1 (64-bit)", MemCfg::Region64, {0xC4, 0xE2, 0x69, 0xDC, 0xC9}, true},
        {"F11 AESENC mem (32-bit non-linear)", MemCfg::NonLinear32, {0x66, 0x0F, 0x38, 0xDC, 0x02}, false},
    };
    constexpr u32 kCopies = 32;
    std::printf("\n%-38s %10s %10s %8s\n", "form", "direct ns", "helper ns", "B/A");
    for (const BenchForm& f : forms) {
        CryptoRig rig;
        if (!rig.init(f.cfg)) {
            ADD_FAILURE() << "configuration unavailable: " << f.name;
            continue;
        }
        put_m128(rig, kData, 0x0F1571C947D9E859ULL, 0x0CB7ADD6AF7F6798ULL);
        std::vector<u8> code;
        for (u32 c = 0; c < kCopies; ++c) code.insert(code.end(), f.insn.begin(), f.insn.end());
        std::vector<double> a, b;
        for (int rep = 0; rep < 5; ++rep) {
            a.push_back(bench_ns_per_insn(rig, code, true, kCopies));
            b.push_back(bench_ns_per_insn(rig, code, false, kCopies));
        }
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        const double ma = a[2], mb = b[2];
        std::printf("%-38s %10.3f %10.3f %8.2f\n", f.name, ma, mb, mb / ma);
        if (f.must_win) EXPECT_LT(ma, mb) << f.name;
    }
}

} // namespace
