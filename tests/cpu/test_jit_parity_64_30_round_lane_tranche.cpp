// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): ROUNDSS/ROUNDSD
// and the VEX.256 lane-wise tranche.

#include "jit_parity_64_fixture.hpp"

// ============================================================================
// ROUNDSS/ROUNDSD native lowering (legacy and VEX), and the VEX.256 lane-wise
// tranche added 2026-10-01. Every case runs under VITREO_JIT_NO_DELEGATE with
// require_jit, so an opcode that stops being lowered natively fails loudly
// instead of silently passing through the interpreter.
// ============================================================================

namespace {

// Encodes a C4-form VEX instruction. `rm` is a register unless `mem_rbx`, in
// which case the r/m operand is [rbx]. `vvvv` is the REGISTER number (the
// field is stored inverted); pass 0 for a form that does not use vvvv.
std::vector<u8> vex_c4(u8 map, u8 pp, bool L, bool W, u8 opcode, u8 reg,
                       u8 vvvv, u8 rm, bool mem_rbx,
                       std::optional<u8> imm = std::nullopt) {
    std::vector<u8> v;
    v.push_back(0xC4);
    u8 b1 = static_cast<u8>(map & 0x1F);
    if (reg < 8) b1 |= 0x80;             // R-bar
    b1 |= 0x40;                          // X-bar (no index)
    if (mem_rbx || rm < 8) b1 |= 0x20;   // B-bar
    v.push_back(b1);
    u8 b2 = static_cast<u8>(((~vvvv & 15) << 3) | (L ? 4 : 0) | (pp & 3));
    if (W) b2 |= 0x80;
    v.push_back(b2);
    v.push_back(opcode);
    if (mem_rbx) {
        v.push_back(static_cast<u8>(0x00 | ((reg & 7) << 3) | 3));  // [rbx]
    } else {
        v.push_back(static_cast<u8>(0xC0 | ((reg & 7) << 3) | (rm & 7)));
    }
    if (imm) v.push_back(*imm);
    return v;
}

// Legacy 66 0F 3A op modrm imm8 (ROUNDSS/SD). r/m register unless mem_rbx.
std::vector<u8> legacy_3a_66(u8 opcode, u8 reg, u8 rm, bool mem_rbx, u8 imm) {
    std::vector<u8> v = {0x66};
    u8 rex = 0;
    if (reg >= 8) rex |= 0x44;
    if (!mem_rbx && rm >= 8) rex |= 0x41;
    if (rex) v.push_back(rex);
    v.push_back(0x0F); v.push_back(0x3A); v.push_back(opcode);
    if (mem_rbx) v.push_back(static_cast<u8>(((reg & 7) << 3) | 3));
    else v.push_back(static_cast<u8>(0xC0 | ((reg & 7) << 3) | (rm & 7)));
    v.push_back(imm);
    return v;
}

constexpr u32 kRoundF32[] = {
    0x3f000000u, 0x3fc00000u, 0x40200000u, 0xbf000000u, 0xbfc00000u,
    0xc0200000u, 0x3effffffu, 0x406ccccdu, 0xc06ccccdu, 0x60ad78ecu,
    0x80000000u, 0x00000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u,
    0x7f800001u, 0x00000001u, 0x4a800001u, 0xcb000000u,
};
constexpr u64 kRoundF64[] = {
    0x3fe0000000000000ull, 0x3ff8000000000000ull, 0x4004000000000000ull,
    0xbfe0000000000000ull, 0xbff8000000000000ull, 0xc004000000000000ull,
    0x3fdfffffffffffffull, 0x400d99999999999aull, 0xc00d99999999999aull,
    0x4415af1d78b58c40ull, 0x8000000000000000ull, 0x0000000000000000ull,
    0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull,
    0x7ff0000000000001ull, 0x0000000000000001ull, 0x4330000000000001ull,
    0x432fffffffffffffull,
};

void seed_round_regs(CpuState& cpu, u8 dst, u8 src1, u8 rm, bool dbl,
                     u64 input, bool rm_is_mem) {
    for (u8 r = 0; r < 16; ++r) {
        for (int i = 0; i < 4; ++i) {
            cpu.sse.xmm[r].u32_[i] = 0x11000000u * (i + 1) + r * 0x010101u;
            cpu.avx.ymm_high[r].u32_[i] = 0x90000000u + 0x100u * i + r;
        }
    }
    (void)dst; (void)src1;
    if (!rm_is_mem) {
        if (dbl) cpu.sse.xmm[rm].u64_[0] = input;
        else cpu.sse.xmm[rm].u32_[0] = static_cast<u32>(input);
    }
}

}  // namespace

struct RoundForm {
    const char* name;
    bool dbl;
    bool vex;
    u8 dst, src1, rm;
    bool mem;
};

// Every imm8, every MXCSR.RC the form can observe, every special input, for
// the register forms that exercise distinct roles (legacy dst!=rm, VEX three
// distinct registers). The interpreter is the reference here; its values
// match silicon row for row (tools/oracle_probe/vex_scalar_round_probe).
TEST_F(JitParity64Test, ScalarRoundParityFullImm8AndInputSweep) {
    const RoundForm forms[] = {
        {"roundss xmm0,xmm2",       false, false, 0, 0, 2, false},
        {"roundsd xmm0,xmm2",       true,  false, 0, 0, 2, false},
        {"vroundss xmm0,xmm1,xmm2", false, true,  0, 1, 2, false},
        {"vroundsd xmm0,xmm1,xmm2", true,  true,  0, 1, 2, false},
    };
    for (const auto& f : forms) {
        for (u32 imm = 0; imm < 16; ++imm) {
            const u32 rcs = (imm & 4) ? 4 : 2;
            for (u32 rci = 0; rci < rcs; ++rci) {
                const u32 rc = (imm & 4) ? rci : (rci ? 3 : 0);
                const usize n = f.dbl ? std::size(kRoundF64) : std::size(kRoundF32);
                for (usize i = 0; i < n; ++i) {
                    const u64 input = f.dbl ? kRoundF64[i] : kRoundF32[i];
                    SCOPED_TRACE(::testing::Message() << f.name << " imm=" << imm
                        << " rc=" << rc << " in=0x" << std::hex << input);
                    m_jit = std::make_unique<JitCompiler>(*m_memory);
                    const u8 op = f.dbl ? 0x0B : 0x0A;
                    std::vector<u8> code = f.vex
                        ? vex_c4(3, 1, false, false, op, f.dst, f.src1, f.rm, false,
                                 static_cast<u8>(imm))
                        : legacy_3a_66(op, f.dst, f.rm, false, static_cast<u8>(imm));
                    check_parity(code.data(), code.size(),
                        [&](CpuState& cpu) {
                            seed_round_regs(cpu, f.dst, f.src1, f.rm, f.dbl, input, false);
                            cpu.sse.mxcsr = 0x1F80u | (rc << 13);
                        }, /*check_flags=*/false, /*check_xmm=*/true,
                        /*require_jit=*/true);
                }
            }
        }
    }
}

// The remaining operand shapes with a reduced input set: dst == r/m (the VEX
// shape the generic bridge could not take), dst == vvvv, all three equal,
// high registers, and the m32/m64 memory source.
TEST_F(JitParity64Test, ScalarRoundParityOperandShapes) {
    const RoundForm forms[] = {
        {"roundss xmm3,xmm3",        false, false, 3, 3, 3, false},
        {"roundsd xmm9,xmm9",        true,  false, 9, 9, 9, false},
        {"roundsd xmm9,xmm10",       true,  false, 9, 9, 10, false},
        {"roundss xmm0,[rbx]",       false, false, 0, 0, 0, true},
        {"roundsd xmm8,[rbx]",       true,  false, 8, 8, 0, true},
        {"vroundss xmm0,xmm1,xmm0",  false, true,  0, 1, 0, false},
        {"vroundsd xmm0,xmm1,xmm0",  true,  true,  0, 1, 0, false},
        {"vroundsd xmm0,xmm0,xmm2",  true,  true,  0, 0, 2, false},
        {"vroundss xmm0,xmm0,xmm0",  false, true,  0, 0, 0, false},
        {"vroundsd xmm8,xmm9,xmm10", true,  true,  8, 9, 10, false},
        {"vroundss xmm8,xmm9,xmm10", false, true,  8, 9, 10, false},
        {"vroundsd xmm0,xmm1,[rbx]", true,  true,  0, 1, 0, true},
        {"vroundss xmm8,xmm9,[rbx]", false, true,  8, 9, 0, true},
    };
    const usize picks[] = {0, 3, 6, 15, 18};  // 0.5, -0.5, just-under-half, SNaN, halfway
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    for (const auto& f : forms) {
        for (u32 imm = 0; imm < 16; ++imm) {
            const u32 rc = (imm & 4) ? 2 : 0;
            for (usize i : picks) {
                const u64 input = f.dbl ? kRoundF64[i] : kRoundF32[i];
                SCOPED_TRACE(::testing::Message() << f.name << " imm=" << imm
                    << " in=0x" << std::hex << input);
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                const u8 op = f.dbl ? 0x0B : 0x0A;
                std::vector<u8> code = f.vex
                    ? vex_c4(3, 1, false, false, op, f.dst, f.src1, f.rm, f.mem,
                             static_cast<u8>(imm))
                    : legacy_3a_66(op, f.dst, f.rm, f.mem, static_cast<u8>(imm));
                if (f.mem) {
                    ASSERT_TRUE(m_memory->write_u64(0x3000, f.dbl ? input
                        : (0xA5A5A5A500000000ull | static_cast<u32>(input))));
                    ASSERT_TRUE(m_memory->write_u64(0x3008, 0x5A5A5A5A5A5A5A5Aull));
                }
                check_parity(code.data(), code.size(),
                    [&](CpuState& cpu) {
                        seed_round_regs(cpu, f.dst, f.src1, f.rm, f.dbl, input, f.mem);
                        cpu.gpr.set64(RBX, 0x3000);
                        cpu.sse.mxcsr = 0x1F80u | (rc << 13);
                    }, /*check_flags=*/false, /*check_xmm=*/true,
                    /*require_jit=*/true);
            }
        }
    }
}

// Absolute values from real silicon (i9-12900KF), not from either engine:
// tools/oracle_probe/vex_scalar_round_probe.expected. DEST pre-filled with
// dead0000/dead1111/dead2222/dead3333, SRC1 (vvvv) with c0ff0000..c0ff3333,
// MXCSR 0x1F80 | RC<<13 with every sticky flag clear.
TEST_F(JitParity64Test, ScalarRoundMatchesSiliconRows) {
    struct Row {
        const char* name; bool dbl; bool vex; u8 imm; u32 rc; u64 input;
        u32 out3, out2, out1, out0; u32 flags;
    };
    const Row rows[] = {
        {"rss imm=1 rc=0 -0.5",     false, false, 1,  0, 0xbf000000u,
            0xdead3333u, 0xdead2222u, 0xdead1111u, 0xbf800000u, 0x20},
        {"rss imm=8 rc=0 SNaN",     false, false, 8,  0, 0x7f800001u,
            0xdead3333u, 0xdead2222u, 0xdead1111u, 0x7fc00001u, 0x01},
        {"rss imm=11 rc=3 2.5",     false, false, 11, 3, 0x40200000u,
            0xdead3333u, 0xdead2222u, 0xdead1111u, 0x40000000u, 0x00},
        {"rsd imm=0 rc=0 -0.5",     true,  false, 0,  0, 0xbfe0000000000000ull,
            0xdead3333u, 0xdead2222u, 0x80000000u, 0x00000000u, 0x20},
        {"rsd imm=0 rc=3 2.5",      true,  false, 0,  3, 0x4004000000000000ull,
            0xdead3333u, 0xdead2222u, 0x40000000u, 0x00000000u, 0x20},
        {"rsd imm=4 rc=2 0.5",      true,  false, 4,  2, 0x3fe0000000000000ull,
            0xdead3333u, 0xdead2222u, 0x3ff00000u, 0x00000000u, 0x20},
        {"vrss imm=2 rc=0 0.49999997", false, true, 2, 0, 0x3effffffu,
            0xc0ff3333u, 0xc0ff2222u, 0xc0ff1111u, 0x3f800000u, 0x20},
        {"vrss imm=12 rc=3 -3.7",   false, true,  12, 3, 0xc06ccccdu,
            0xc0ff3333u, 0xc0ff2222u, 0xc0ff1111u, 0xc0400000u, 0x00},
        {"vrsd imm=0 rc=0 SNaN",    true,  true,  0,  0, 0x7ff0000000000001ull,
            0xc0ff3333u, 0xc0ff2222u, 0x7ff80000u, 0x00000001u, 0x01},
        {"vrsd imm=5 rc=1 2^52-0.5", true, true,  5,  1, 0x432fffffffffffffull,
            0xc0ff3333u, 0xc0ff2222u, 0x432fffffu, 0xfffffffeu, 0x20},
        {"vrsd imm=9 rc=0 0.5",     true,  true,  9,  0, 0x3fe0000000000000ull,
            0xc0ff3333u, 0xc0ff2222u, 0x00000000u, 0x00000000u, 0x00},
    };
    for (const auto& r : rows) {
        SCOPED_TRACE(r.name);
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        const u8 op = r.dbl ? 0x0B : 0x0A;
        std::vector<u8> code = r.vex
            ? vex_c4(3, 1, false, false, op, 0, 1, 2, false, r.imm)
            : legacy_3a_66(op, 0, 2, false, r.imm);
        check_parity(code.data(), code.size(),
            [&](CpuState& cpu) {
                for (int i = 0; i < 4; ++i) {
                    cpu.sse.xmm[0].u32_[i] = 0xdead0000u + 0x1111u * i;
                    cpu.sse.xmm[1].u32_[i] = 0xc0ff0000u + 0x1111u * i;
                    cpu.sse.xmm[2].u32_[i] = 0x5ec00000u + 0x11111u * i;
                    cpu.avx.ymm_high[0].u32_[i] = 0xbad00000u + i;
                }
                if (r.dbl) cpu.sse.xmm[2].u64_[0] = r.input;
                else cpu.sse.xmm[2].u32_[0] = static_cast<u32>(r.input);
                cpu.sse.mxcsr = 0x1F80u | (r.rc << 13);
            }, /*check_flags=*/false, /*check_xmm=*/true, /*require_jit=*/true);
        ASSERT_TRUE(m_last_parity_ok);
        for (CpuState* cpu : {&m_last_jit_cpu, &m_last_interp_cpu}) {
            EXPECT_EQ(cpu->sse.xmm[0].u32_[0], r.out0);
            EXPECT_EQ(cpu->sse.xmm[0].u32_[1], r.out1);
            EXPECT_EQ(cpu->sse.xmm[0].u32_[2], r.out2);
            EXPECT_EQ(cpu->sse.xmm[0].u32_[3], r.out3);
            EXPECT_EQ(cpu->sse.mxcsr & 0x3Fu, r.flags);
            if (r.vex) {
                for (int i = 0; i < 4; ++i) EXPECT_EQ(cpu->avx.ymm_high[0].u32_[i], 0u);
            } else {
                for (int i = 0; i < 4; ++i)
                    EXPECT_EQ(cpu->avx.ymm_high[0].u32_[i], 0xbad00000u + i);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// VEX.256 lane-wise tranche
// ---------------------------------------------------------------------------

namespace {

struct LaneOp {
    const char* name;
    u8 map, pp, opcode;
    bool unary;        // vvvv unused (must encode 1111b)
    bool fp;           // seed finite floats instead of integer bit patterns
    bool has_imm;
    bool shift_count;  // r/m is a 128-bit shift count shared by both lanes
};

const LaneOp kLaneTranche[] = {
    {"vpunpcklbw",  1, 1, 0x60, false, false, false, false},
    {"vpunpckhbw",  1, 1, 0x68, false, false, false, false},
    {"vpunpcklwd",  1, 1, 0x61, false, false, false, false},
    {"vpunpckhwd",  1, 1, 0x69, false, false, false, false},
    {"vpunpckldq",  1, 1, 0x62, false, false, false, false},
    {"vpunpckhdq",  1, 1, 0x6A, false, false, false, false},
    {"vpunpcklqdq", 1, 1, 0x6C, false, false, false, false},
    {"vpunpckhqdq", 1, 1, 0x6D, false, false, false, false},
    {"vunpcklps",   1, 0, 0x14, false, true,  false, false},
    {"vunpckhps",   1, 0, 0x15, false, true,  false, false},
    {"vunpcklpd",   1, 1, 0x14, false, true,  false, false},
    {"vunpckhpd",   1, 1, 0x15, false, true,  false, false},
    {"vpacksswb",   1, 1, 0x63, false, false, false, false},
    {"vpackssdw",   1, 1, 0x6B, false, false, false, false},
    {"vpackuswb",   1, 1, 0x67, false, false, false, false},
    {"vpackusdw",   2, 1, 0x2B, false, false, false, false},
    {"vpshufd",     1, 1, 0x70, true,  false, true,  false},
    {"vpshuflw",    1, 3, 0x70, true,  false, true,  false},
    {"vpshufhw",    1, 2, 0x70, true,  false, true,  false},
    {"vpmulhw",     1, 1, 0xE5, false, false, false, false},
    {"vpmulhuw",    1, 1, 0xE4, false, false, false, false},
    {"vpmulhrsw",   2, 1, 0x0B, false, false, false, false},
    {"vpmuludq",    1, 1, 0xF4, false, false, false, false},
    {"vpmuldq",     2, 1, 0x28, false, false, false, false},
    {"vpsadbw",     1, 1, 0xF6, false, false, false, false},
    {"vpaddsb",     1, 1, 0xEC, false, false, false, false},
    {"vpaddsw",     1, 1, 0xED, false, false, false, false},
    {"vpsubsb",     1, 1, 0xE8, false, false, false, false},
    {"vpsubsw",     1, 1, 0xE9, false, false, false, false},
    {"vpsignb",     2, 1, 0x08, false, false, false, false},
    {"vpsignw",     2, 1, 0x09, false, false, false, false},
    {"vpsignd",     2, 1, 0x0A, false, false, false, false},
    {"vpabsb",      2, 1, 0x1C, true,  false, false, false},
    {"vpabsw",      2, 1, 0x1D, true,  false, false, false},
    {"vpabsd",      2, 1, 0x1E, true,  false, false, false},
    {"vpmaddubsw",  2, 1, 0x04, false, false, false, false},
    {"vpmaddwd",    1, 1, 0xF5, false, false, false, false},
    {"vphaddw",     2, 1, 0x01, false, false, false, false},
    {"vphaddd",     2, 1, 0x02, false, false, false, false},
    {"vphaddsw",    2, 1, 0x03, false, false, false, false},
    {"vphsubw",     2, 1, 0x05, false, false, false, false},
    {"vphsubd",     2, 1, 0x06, false, false, false, false},
    {"vphsubsw",    2, 1, 0x07, false, false, false, false},
    {"vhaddps",     1, 3, 0x7C, false, true,  false, false},
    {"vhaddpd",     1, 1, 0x7C, false, true,  false, false},
    {"vhsubps",     1, 3, 0x7D, false, true,  false, false},
    {"vhsubpd",     1, 1, 0x7D, false, true,  false, false},
    {"vsqrtps",     1, 0, 0x51, true,  true,  false, false},
    {"vsqrtpd",     1, 1, 0x51, true,  true,  false, false},
    {"vmovsldup",   1, 2, 0x12, true,  true,  false, false},
    {"vmovshdup",   1, 2, 0x16, true,  true,  false, false},
    {"vmovddup",    1, 3, 0x12, true,  true,  false, false},
    {"vpblendw",    3, 1, 0x0E, false, false, true,  false},
    {"vblendps",    3, 1, 0x0C, false, true,  true,  false},
    {"vblendpd",    3, 1, 0x0D, false, true,  true,  false},
    {"vcvtdq2ps",   1, 0, 0x5B, true,  false, false, false},
    {"vpsrlw",      1, 1, 0xD1, false, false, false, true},
    {"vpsrld",      1, 1, 0xD2, false, false, false, true},
    {"vpsrlq",      1, 1, 0xD3, false, false, false, true},
    {"vpsraw",      1, 1, 0xE1, false, false, false, true},
    {"vpsrad",      1, 1, 0xE2, false, false, false, true},
    {"vpsllw",      1, 1, 0xF1, false, false, false, true},
    {"vpslld",      1, 1, 0xF2, false, false, false, true},
    {"vpsllq",      1, 1, 0xF3, false, false, false, true},
};

// Distinct, asymmetric per register, lane and byte: swapping any two operand
// roles or lanes changes the result. Integer seeds include sign-set bytes and
// saturation candidates; FP seeds are finite, positive for sqrt, with
// magnitudes that make horizontal pairs and blends distinguishable.
void seed_lane_regs(CpuState& cpu, bool fp, u32 salt) {
    for (u8 r = 0; r < 16; ++r) {
        for (int b = 0; b < 16; ++b) {
            const u32 k = static_cast<u32>(r) * 37u + static_cast<u32>(b) * 11u + salt;
            cpu.sse.xmm[r].u8_[b] = static_cast<u8>((k * 0x9Eu) ^ (b << 4) ^ r);
            cpu.avx.ymm_high[r].u8_[b] = static_cast<u8>((k * 0x61u) + 0x80u + r);
        }
        // A few saturation candidates per register.
        cpu.sse.xmm[r].u16_[1] = 0x7FFF; cpu.sse.xmm[r].u16_[2] = 0x8000;
        cpu.avx.ymm_high[r].u16_[5] = 0x8000; cpu.avx.ymm_high[r].u16_[6] = 0x7FFF;
        if (fp) {
            for (int i = 0; i < 4; ++i) {
                cpu.sse.xmm[r].f32_[i] = 0.75f * static_cast<float>(i + 1) +
                                         0.125f * static_cast<float>(r) +
                                         static_cast<float>(salt & 3);
                cpu.avx.ymm_high[r].f32_[i] = 2.5f * static_cast<float>(i + 1) +
                                              0.0625f * static_cast<float>(r + 1);
            }
            // Keep the pd view finite and positive too (sqrt/hadd).
            cpu.sse.xmm[r].f64_[0] = 1.5 + 0.25 * r + (salt & 3);
            cpu.sse.xmm[r].f64_[1] = 42.0 + 3.0 * r;
            cpu.avx.ymm_high[r].f64_[0] = 7.75 + 0.5 * r;
            cpu.avx.ymm_high[r].f64_[1] = 1024.5 + r;
        }
    }
}

}  // namespace

TEST_F(JitParity64Test, Vex256LaneTrancheRegisterForms) {
    struct Regs { u8 dst, vvvv, rm; };
    const Regs combos[] = {
        {0, 1, 2},   // three distinct
        {0, 1, 0},   // dst == r/m  (the merge-copy trap)
        {0, 0, 2},   // dst == vvvv
        {2, 2, 2},   // all equal
        {8, 9, 10},  // high registers (REX.R / REX.B through VEX)
        {3, 11, 3},  // dst == r/m with a high vvvv
    };
    const u8 imms[] = {0x00, 0x1B, 0x5A, 0xA5, 0xE4, 0xFF};
    const u64 counts[] = {0, 3, 7, 15, 16, 31, 32, 63, 64, 0x100000000ull};
    for (const auto& op : kLaneTranche) {
        for (const auto& rg : combos) {
            const u8 vvvv = op.unary ? 0 : rg.vvvv;
            const usize imm_n = op.has_imm ? std::size(imms) : 1;
            const usize cnt_n = op.shift_count ? std::size(counts) : 1;
            for (usize ii = 0; ii < imm_n; ++ii) {
                for (usize ci = 0; ci < cnt_n; ++ci) {
                    SCOPED_TRACE(::testing::Message() << op.name << " ymm" << +rg.dst
                        << ",ymm" << +vvvv << ",ymm" << +rg.rm
                        << (op.has_imm ? " imm=" : "") << (op.has_imm ? +imms[ii] : 0)
                        << (op.shift_count ? " count=" : "")
                        << (op.shift_count ? counts[ci] : 0));
                    m_jit = std::make_unique<JitCompiler>(*m_memory);
                    std::vector<u8> code = vex_c4(op.map, op.pp, /*L=*/true, false,
                        op.opcode, rg.dst, vvvv, rg.rm, false,
                        op.has_imm ? std::optional<u8>(imms[ii]) : std::nullopt);
                    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
                    check_parity(code.data(), code.size(),
                        [&](CpuState& cpu) {
                            seed_lane_regs(cpu, op.fp, static_cast<u32>(ii + ci));
                            if (op.shift_count) {
                                cpu.sse.xmm[rg.rm].u64_[0] = counts[ci];
                                // The high lane of the count register must be
                                // ignored: poison it.
                                cpu.avx.ymm_high[rg.rm].u64_[0] = 0xFFFFFFFFFFFFFFFFull;
                            }
                        }, /*check_flags=*/false, /*check_xmm=*/true,
                        /*require_jit=*/true);
                }
            }
        }
    }
}

TEST_F(JitParity64Test, Vex256LaneTrancheMemoryForms) {
    const u8 imms[] = {0x00, 0x5A, 0xC3};
    const u64 counts[] = {0, 5, 16, 33, 64};
    for (const auto& op : kLaneTranche) {
        for (u8 dst : {u8{0}, u8{9}}) {
            const u8 vvvv = op.unary ? 0 : static_cast<u8>(dst == 0 ? 1 : 12);
            const usize imm_n = op.has_imm ? std::size(imms) : 1;
            const usize cnt_n = op.shift_count ? std::size(counts) : 1;
            for (usize ii = 0; ii < imm_n; ++ii) {
                for (usize ci = 0; ci < cnt_n; ++ci) {
                    SCOPED_TRACE(::testing::Message() << op.name << " ymm" << +dst
                        << ",ymm" << +vvvv << ",[rbx]"
                        << (op.has_imm ? " imm=" : "") << (op.has_imm ? +imms[ii] : 0)
                        << (op.shift_count ? " count=" : "")
                        << (op.shift_count ? counts[ci] : 0));
                    m_jit = std::make_unique<JitCompiler>(*m_memory);
                    std::vector<u8> code = vex_c4(op.map, op.pp, /*L=*/true, false,
                        op.opcode, dst, vvvv, 0, /*mem_rbx=*/true,
                        op.has_imm ? std::optional<u8>(imms[ii]) : std::nullopt);
                    // 32 bytes of operand at 0x3000, distinct from every
                    // register seed. Shift counts: the low qword, with the
                    // bytes 16..31 poisoned (must never be read as a count).
                    for (u32 b = 0; b < 32; ++b) {
                        u8 byte = static_cast<u8>((b * 0x47u + 0x13u + ii) ^ (b >> 2));
                        if (op.fp) {
                            // finite small-magnitude floats/doubles
                            static const u8 kF32[16] = {
                                0x00,0x00,0x40,0x40, 0x00,0x00,0xa0,0x40,
                                0x00,0x00,0x10,0x41, 0xcd,0xcc,0x4c,0x3e};
                            static const u8 kF32hi[16] = {
                                0x00,0x00,0x80,0x3f, 0x00,0x00,0xf0,0x41,
                                0x00,0x00,0x20,0x41, 0x00,0x00,0x00,0x40};
                            byte = b < 16 ? kF32[b] : kF32hi[b - 16];
                        }
                        ASSERT_TRUE(m_memory->write_u8(0x3000 + b, byte));
                    }
                    if (op.shift_count) {
                        ASSERT_TRUE(m_memory->write_u64(0x3000, counts[ci]));
                        ASSERT_TRUE(m_memory->write_u64(0x3010, 0xFFFFFFFFFFFFFFFFull));
                    }
                    if (op.fp && (op.opcode == 0x51 || op.pp == 1)) {
                        // pd/sqrt forms: finite positive doubles in both lanes.
                        ASSERT_TRUE(m_memory->write_u64(0x3000, 0x4008000000000000ull));  // 3.0
                        ASSERT_TRUE(m_memory->write_u64(0x3008, 0x4022000000000000ull));  // 9.0
                        ASSERT_TRUE(m_memory->write_u64(0x3010, 0x3ff8000000000000ull));  // 1.5
                        ASSERT_TRUE(m_memory->write_u64(0x3018, 0x4044000000000000ull));  // 40.0
                    }
                    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
                    check_parity(code.data(), code.size(),
                        [&](CpuState& cpu) {
                            seed_lane_regs(cpu, op.fp, static_cast<u32>(ii + 2 * ci));
                            cpu.gpr.set64(RBX, 0x3000);
                        }, /*check_flags=*/false, /*check_xmm=*/true,
                        /*require_jit=*/true);
                }
            }
        }
    }
}

// The VEX.128 alias shape `op xmm0, xmm1, xmm0` (dst == r/m != vvvv): the
// generic bridge's merge copy would destroy the second source, so these forms
// now run the lane split's low pass. Every opcode in the tranche, both roles.
TEST_F(JitParity64Test, Vex128AliasShapeOfLaneTrancheIsNative) {
    for (const auto& op : kLaneTranche) {
        if (op.unary) continue;
        for (u8 dst : {u8{0}, u8{10}}) {
            const u8 vvvv = static_cast<u8>(dst == 0 ? 1 : 11);
            SCOPED_TRACE(::testing::Message() << op.name << " xmm" << +dst
                << ",xmm" << +vvvv << ",xmm" << +dst);
            m_jit = std::make_unique<JitCompiler>(*m_memory);
            std::vector<u8> code = vex_c4(op.map, op.pp, /*L=*/false, false,
                op.opcode, dst, vvvv, dst, false,
                op.has_imm ? std::optional<u8>(0x5A) : std::nullopt);
            ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
            check_parity(code.data(), code.size(),
                [&](CpuState& cpu) {
                    seed_lane_regs(cpu, op.fp, 7);
                    if (op.shift_count) cpu.sse.xmm[dst].u64_[0] = 9;
                }, /*check_flags=*/false, /*check_xmm=*/true,
                /*require_jit=*/true);
        }
    }
}

// The ISA matrix's seed 1 -- every XMM register holding {+inf, -inf, QNaN,
// SNaN} across its four single lanes -- with the source's OTHER lanes
// non-finite. The scalar forms must look at lane 0 only: a NaN in lane 3 is
// not this instruction's business, for PE or for IE.
TEST_F(JitParity64Test, ScalarRoundIgnoresNonFiniteUpperSourceLanes) {
    for (bool dbl : {false, true}) {
        for (bool vex : {false, true}) {
            for (u32 imm = 0; imm < 16; ++imm) {
                SCOPED_TRACE(::testing::Message() << (dbl ? "sd" : "ss")
                    << (vex ? " vex" : " legacy") << " imm=" << imm);
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                const u8 op = dbl ? 0x0B : 0x0A;
                std::vector<u8> code = vex
                    ? vex_c4(3, 1, false, false, op, 0, 1, 2, false, static_cast<u8>(imm))
                    : legacy_3a_66(op, 0, 1, false, static_cast<u8>(imm));
                check_parity(code.data(), code.size(),
                    [&](CpuState& cpu) {
                        for (u8 r = 0; r < 16; ++r) {
                            cpu.sse.xmm[r].u32_[0] = 0x7f800000u;
                            cpu.sse.xmm[r].u32_[1] = 0xff800000u;
                            cpu.sse.xmm[r].u32_[2] = 0x7fc00001u;
                            cpu.sse.xmm[r].u32_[3] = 0x7f800001u;
                        }
                        cpu.sse.mxcsr = 0x1F80u;
                    }, /*check_flags=*/false, /*check_xmm=*/true,
                    /*require_jit=*/true);
                ASSERT_TRUE(m_last_parity_ok);
                // Silicon: +Inf rounds to itself with no flag at all
                // (vex_scalar_round_probe.expected, in=7f800000 rows).
                if (!dbl) EXPECT_EQ(m_last_jit_cpu.sse.mxcsr, 0x1F80u);
            }
        }
    }
}

// VEX.256 full-width LOADS through the lane-split driver (the path every
// 32-bit guest takes, and a 64-bit one without the linear64 wide access):
// both lanes, aligned and unaligned mnemonics, high register, and the
// 32-byte-aligned-only rule for VMOVDQA/VMOVAPS/VMOVAPD.
TEST_F(JitParity64Test, Vex256FullWidthLoadsThroughLaneSplit) {
    struct Row { const char* name; u8 pp, opcode; bool aligned_only; };
    const Row rows[] = {
        {"vmovdqa ymm,[rbx]", 1, 0x6F, true},
        {"vmovaps ymm,[rbx]", 0, 0x28, true},
        {"vmovapd ymm,[rbx]", 1, 0x28, true},
        {"vmovdqu ymm,[rbx]", 2, 0x6F, false},
        {"vmovups ymm,[rbx]", 0, 0x10, false},
        {"vmovupd ymm,[rbx]", 1, 0x10, false},
    };
    for (const auto& r : rows) {
        for (u8 dst : {u8{0}, u8{13}}) {
            for (u64 base : {u64{0x3000}, u64{0x3010}, u64{0x3001}}) {
                SCOPED_TRACE(::testing::Message() << r.name << " dst=" << +dst
                    << " base=0x" << std::hex << base);
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                std::vector<u8> code = vex_c4(1, r.pp, /*L=*/true, false,
                    r.opcode, dst, 0, 0, /*mem_rbx=*/true);
                for (u32 b = 0; b < 64; ++b) {
                    ASSERT_TRUE(m_memory->write_u8(0x3000 + b,
                        static_cast<u8>(0xA0 + b * 3)));
                }
                // Misaligned m256 on the aligned mnemonics: both engines must
                // raise (#GP(0)); check_parity compares the fault outcome.
                check_parity(code.data(), code.size(),
                    [&](CpuState& cpu) {
                        seed_lane_regs(cpu, false, 3);
                        cpu.gpr.set64(RBX, base);
                    }, /*check_flags=*/false, /*check_xmm=*/true,
                    /*require_jit=*/!(r.aligned_only && (base & 31)));
            }
        }
    }
}
