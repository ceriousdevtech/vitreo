// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): VEX-encoded SSE and VEX.256
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

// ---------------------------------------------------------------------------
// VEX-encoded SSE: the JIT must not lower these as the legacy two-operand form
//
// A VEX form is three-operand (dst := vvvv OP rm) and zeroes YMM[255:128] of
// the destination (Intel SDM Vol. 2, "VEX Encoding"). Lowering it as the
// legacy `dst OP= rm` silently computes into the wrong register — which is how
// BoringSSL's AVX SHA-256/GHASH paths produced wrong key material and every
// in-guest TLS 1.3 handshake failed with bad_record_mac.
// ---------------------------------------------------------------------------
namespace {
void seed_xmm_pattern(CpuState& cpu) {
    for (int r = 0; r < 8; ++r)
        for (int b = 0; b < 16; ++b)
            cpu.sse.xmm[r].u8_[b] = static_cast<u8>((r * 0x11) + b + 1);
    for (int r = 0; r < 8; ++r)
        for (int b = 0; b < 16; ++b)
            cpu.avx.ymm_high[r].u8_[b] = static_cast<u8>(0xA0 + r);
}
}  // namespace

TEST_F(JitParityTest, VexThreeOperandUsesVvvvNotDestination) {
    // Every one of these has dst != vvvv != rm, so a legacy lowering computes
    // `xmm0 OP= xmm2` instead of `xmm1 OP xmm2` and the divergence is visible.
    struct Case { const char* name; std::vector<u8> code; };
    const std::vector<Case> cases = {
        // VEX.128.66.0F.WIG F4 /r  VPMULUDQ xmm0, xmm1, xmm2
        {"vpmuludq", {0xC5, 0xF1, 0xF4, 0xC2}},
        // VEX.128.66.0F.WIG F5 /r  VPMADDWD xmm0, xmm1, xmm2
        {"vpmaddwd", {0xC5, 0xF1, 0xF5, 0xC2}},
        // VEX.128.66.0F.WIG 67 /r  VPACKUSWB xmm0, xmm1, xmm2
        {"vpackuswb", {0xC5, 0xF1, 0x67, 0xC2}},
        // VEX.128.66.0F.WIG FE /r  VPADDD xmm0, xmm1, xmm2
        {"vpaddd", {0xC5, 0xF1, 0xFE, 0xC2}},
        // VEX.128.66.0F.WIG EF /r  VPXOR xmm0, xmm1, xmm2
        {"vpxor", {0xC5, 0xF1, 0xEF, 0xC2}},
        // VEX.128.66.0F38.WIG 00 /r  VPSHUFB xmm0, xmm1, xmm2
        {"vpshufb", {0xC4, 0xE2, 0x71, 0x00, 0xC2}},
        // VEX.128.66.0F3A.WIG 0F /r ib  VPALIGNR xmm0, xmm1, xmm2, 4
        {"vpalignr", {0xC4, 0xE3, 0x71, 0x0F, 0xC2, 0x04}},
        // VEX.128.66.0F3A.WIG 0E /r ib  VPBLENDW xmm0, xmm1, xmm2, 0xC3
        {"vpblendw", {0xC4, 0xE3, 0x71, 0x0E, 0xC2, 0xC3}},
        // VEX.128.66.0F.WIG 68 /r  VPUNPCKHBW xmm0, xmm1, xmm2
        {"vpunpckhbw", {0xC5, 0xF1, 0x68, 0xC2}},
        // VEX.128.66.0F38.WIG 40 /r  VPMULLD xmm0, xmm1, xmm2
        {"vpmulld", {0xC4, 0xE2, 0x71, 0x40, 0xC2}},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        check_parity(c.code.data(), c.code.size(), seed_xmm_pattern, false, true);
    }
}

TEST_F(JitParityTest, VexNddShiftImmediateWritesVvvv) {
    // VEX.NDD.128.66.0F 73 /3 ib — VPSRLDQ xmm0, xmm1, 6. ModRM.reg is the
    // opcode extension, so the destination is vvvv (xmm0) and the source is
    // the r/m register (xmm1): the legacy in-place shift would rewrite xmm1
    // and never touch xmm0.
    check_parity({0xC5, 0xF9, 0x73, 0xD9, 0x06}, seed_xmm_pattern, false, true);
    // VPSLLDQ xmm0, xmm1, 6 (/7) and VPSRLD xmm0, xmm1, 11 (72 /2).
    check_parity({0xC5, 0xF9, 0x73, 0xF9, 0x06}, seed_xmm_pattern, false, true);
    check_parity({0xC5, 0xF9, 0x72, 0xD1, 0x0B}, seed_xmm_pattern, false, true);
}

TEST_F(JitParityTest, Vex128ZeroesDestinationUpperLane) {
    // The 128-bit VEX form must clear YMM[255:128]; check_parity compares the
    // XMM file, so assert the high lane directly against the interpreter.
    const std::vector<u8> code = {0xC5, 0xF1, 0xFE, 0xC2};  // VPADDD xmm0,xmm1,xmm2
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
    BasicBlock block = build_block(code.data(), code.size());
    ASSERT_FALSE(block.instructions.empty());

    CpuState interp_cpu{};
    interp_cpu.mode = CpuMode::Protected;
    interp_cpu.rip = 0x1000;
    interp_cpu.gpr.rsp = 0x8000;
    seed_xmm_pattern(interp_cpu);
    {
        Interpreter interp(interp_cpu, *m_memory);
        ASSERT_EQ(interp.run(block.instruction_count), ExecResult::Continue);
    }
    for (int b = 0; b < 16; ++b) {
        EXPECT_EQ(interp_cpu.avx.ymm_high[0].u8_[b], 0u)
            << "interpreter: byte " << b << " of YMM0[255:128] not zeroed";
    }

    CpuState jit_cpu{};
    jit_cpu.mode = CpuMode::Protected;
    jit_cpu.rip = 0x1000;
    jit_cpu.gpr.rsp = 0x8000;
    seed_xmm_pattern(jit_cpu);
    auto* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    test::execute_like_dispatcher(*m_jit, *compiled, jit_cpu);

    for (int b = 0; b < 16; ++b) {
        EXPECT_EQ(jit_cpu.avx.ymm_high[0].u8_[b], 0u)
            << "byte " << b << " of YMM0[255:128] not zeroed";
    }
    // A register the instruction did not write keeps its upper lane.
    EXPECT_EQ(jit_cpu.avx.ymm_high[1].u8_[0], 0xA1u);
}

// ---------------------------------------------------------------------------
// VEX.256 lane split in a 32-BIT block
//
// JitCompiler::compile_vex256_lane_split runs the existing 128-bit lowering
// twice -- once over sse.xmm[], once over avx.ymm_high[] -- for the
// element-wise forms on its allowlist. It is driven from compile_sse, NOT from
// compile_64bit, so a 32-bit (compatibility mode) block gets the split too.
// VEX.256 is architecturally legal in compatibility mode and BoringSSL emits
// it from 32-bit code, so this is a live path for Battle.net/Chrome/Steam.
//
// Every test here is check_native_parity: with VITREO_JIT_NO_DELEGATE set, a
// lowering that quietly hands the instruction back to the interpreter fails to
// compile instead of silently satisfying the value comparison. And check_xmm
// now diffs avx.ymm_high, so a stale or lane-blind high half is visible.
// ---------------------------------------------------------------------------
namespace {
/// Distinct data in BOTH lanes, with the high lane deliberately not a function
/// of the low lane: a lowering that computes the low result twice, or that
/// leaves YMM[255:128] untouched, must show up in the diff.
void seed_ymm_lanes(CpuState& cpu) {
    for (int r = 0; r < 8; ++r) {
        for (int b = 0; b < 16; ++b) {
            cpu.sse.xmm[r].u8_[b] = static_cast<u8>((r * 0x11) + b + 1);
            cpu.avx.ymm_high[r].u8_[b] =
                static_cast<u8>(0x80 ^ ((r * 0x17) + (b * 3) + 5));
        }
    }
}

/// Ordered finite floats -- no NaN/Inf -- so MINPS/MAXPS/ANDPS select a
/// different operand per element and the high lane's answer differs from the
/// low lane's.
void seed_ymm_floats(CpuState& cpu) {
    for (int r = 0; r < 8; ++r) {
        for (int e = 0; e < 4; ++e) {
            const float lo = static_cast<float>((r + 1) * 4 + e) * 0.5f;
            const float hi = static_cast<float>((8 - r) * 4 + e) * -1.5f;
            std::memcpy(&cpu.sse.xmm[r].u32_[e], &lo, sizeof(float));
            std::memcpy(&cpu.avx.ymm_high[r].u32_[e], &hi, sizeof(float));
        }
    }
}
}  // namespace

// Every opcode on vex256_lane_split_allowed()'s list, VEX.256 register form,
// dst/vvvv/rm all distinct (ymm0 := ymm1 OP ymm2). 2-byte VEX (C5) with
// R=1, vvvv=~1, L=1; pp=01 for the 66-prefixed forms and pp=00 for the
// 0F-only ones.
TEST_F(JitParityTest, Native32BitVex256LaneSplitWritesBothLanes) {
    struct Case { const char* name; std::vector<u8> code; bool floats; };
    const std::vector<Case> cases = {
        // VEX.256.66.0F.WIG EF /r  VPXOR ymm0, ymm1, ymm2
        {"vpxor",  {0xC5, 0xF5, 0xEF, 0xC2}, false},
        // VEX.256.66.0F.WIG DB /r  VPAND ymm0, ymm1, ymm2
        {"vpand",  {0xC5, 0xF5, 0xDB, 0xC2}, false},
        // VEX.256.66.0F.WIG EB /r  VPOR ymm0, ymm1, ymm2
        {"vpor",   {0xC5, 0xF5, 0xEB, 0xC2}, false},
        // VEX.256.0F.WIG 54 /r     VANDPS ymm0, ymm1, ymm2
        {"vandps", {0xC5, 0xF4, 0x54, 0xC2}, false},
        // VEX.256.66.0F.WIG FE /r  VPADDD ymm0, ymm1, ymm2
        {"vpaddd", {0xC5, 0xF5, 0xFE, 0xC2}, false},
        // VEX.256.66.0F.WIG FA /r  VPSUBD ymm0, ymm1, ymm2
        {"vpsubd", {0xC5, 0xF5, 0xFA, 0xC2}, false},
        // VEX.256.0F.WIG 5D /r     VMINPS ymm0, ymm1, ymm2
        {"vminps", {0xC5, 0xF4, 0x5D, 0xC2}, true},
        // VEX.256.0F.WIG 5F /r     VMAXPS ymm0, ymm1, ymm2
        {"vmaxps", {0xC5, 0xF4, 0x5F, 0xC2}, true},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        check_native_parity(c.code.data(), c.code.size(),
                            c.floats ? seed_ymm_floats : seed_ymm_lanes,
                            false, true);
    }
}

// Parity with the interpreter is only as good as the interpreter. Pin the
// architectural answer for one form outright, so a lane split that is wrong in
// exactly the same way on both paths cannot hide.
TEST_F(JitParityTest, Native32BitVex256PadddAbsoluteLaneValues) {
    ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    // VPADDD ymm0, ymm1, ymm2
    const std::vector<u8> code = {0xC5, 0xF5, 0xFE, 0xC2};
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
    BasicBlock block = build_block(code.data(), code.size());
    ASSERT_EQ(block.instruction_count, 1u);

    CpuState cpu;
    init_cpu(cpu);
    for (int e = 0; e < 4; ++e) {
        cpu.sse.xmm[1].u32_[e]      = 0x10000000u + static_cast<u32>(e);
        cpu.sse.xmm[2].u32_[e]      = 0x00000100u * static_cast<u32>(e + 1);
        cpu.avx.ymm_high[1].u32_[e] = 0xFFFFFFF0u + static_cast<u32>(e);
        cpu.avx.ymm_high[2].u32_[e] = 0x00000011u * static_cast<u32>(e + 1);
        // Destination starts non-zero in both lanes so an unwritten half is
        // not mistaken for a correct zero.
        cpu.sse.xmm[0].u32_[e]      = 0xDEADBEEFu;
        cpu.avx.ymm_high[0].u32_[e] = 0xCAFEBABEu;
    }

    JitCompiler jit(*m_memory);
    CompiledBlock* cb = jit.compile(block);
    ASSERT_NE(cb, nullptr) << "no native 32-bit VEX.256 lane split";
    ASSERT_EQ(test::execute_like_dispatcher(jit, *cb, cpu), JitExecResult::Continue);

    for (int e = 0; e < 4; ++e) {
        // PADDD wraps per 32-bit element and never carries across elements or
        // across the lane boundary (Intel SDM Vol. 2, PADDD).
        const u32 want_lo = (0x10000000u + static_cast<u32>(e)) +
                            (0x00000100u * static_cast<u32>(e + 1));
        const u32 want_hi = (0xFFFFFFF0u + static_cast<u32>(e)) +
                            (0x00000011u * static_cast<u32>(e + 1));
        EXPECT_EQ(cpu.sse.xmm[0].u32_[e], want_lo)
            << "YMM0 low lane element " << e;
        EXPECT_EQ(cpu.avx.ymm_high[0].u32_[e], want_hi)
            << "YMM0 high lane element " << e;
    }
    // Sources untouched: VEX is non-destructive.
    for (int e = 0; e < 4; ++e) {
        EXPECT_EQ(cpu.sse.xmm[1].u32_[e], 0x10000000u + static_cast<u32>(e));
        EXPECT_EQ(cpu.avx.ymm_high[1].u32_[e],
                  0xFFFFFFF0u + static_cast<u32>(e));
    }
}

// The 128-bit VEX form must ZERO YMM[255:128] of the destination (Intel SDM
// Vol. 2, 2.3.5). The lane-split driver only claims VEX.L forms, so this is
// the neighbouring path -- and the one that regresses if the split ever starts
// suppressing the generic high-lane zero for the 128-bit case too.
TEST_F(JitParityTest, Native32BitVex128ZeroesHighLane) {
    struct Case { const char* name; std::vector<u8> code; };
    const std::vector<Case> cases = {
        {"vpxor",  {0xC5, 0xF1, 0xEF, 0xC2}},  // VPXOR xmm0, xmm1, xmm2
        {"vpaddd", {0xC5, 0xF1, 0xFE, 0xC2}},  // VPADDD xmm0, xmm1, xmm2
        {"vandps", {0xC5, 0xF0, 0x54, 0xC2}},  // VANDPS xmm0, xmm1, xmm2
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        // seed_ymm_lanes leaves YMM0[255:128] non-zero, so "already zero"
        // cannot pass by accident.
        check_native_parity(c.code.data(), c.code.size(), seed_ymm_lanes,
                            false, true);
    }
}

// dst == the r/m source. The lane pass rewrites xmm[dst]/ymm_high[dst] in
// place, so lane 1 must not read a destination that lane 0 already clobbered,
// and the driver must not zero ymm_high[dst] between the lanes.
TEST_F(JitParityTest, Native32BitVex256DstAliasesRmSource) {
    struct Case { const char* name; std::vector<u8> code; };
    const std::vector<Case> cases = {
        // VPADDD ymm0, ymm1, ymm0 -- dst is also the r/m source
        {"vpaddd ymm0,ymm1,ymm0", {0xC5, 0xF5, 0xFE, 0xC0}},
        // VPSUBD ymm0, ymm1, ymm0 -- non-commutative, so operand order shows
        {"vpsubd ymm0,ymm1,ymm0", {0xC5, 0xF5, 0xFA, 0xC0}},
        // VPXOR ymm1, ymm1, ymm1 -- dst == vvvv == rm, the idiomatic zeroing
        // form; both lanes must end up zero.
        {"vpxor ymm1,ymm1,ymm1",  {0xC5, 0xF5, 0xEF, 0xC9}},
        // VPAND ymm2, ymm1, ymm2
        {"vpand ymm2,ymm1,ymm2",  {0xC5, 0xF5, 0xDB, 0xD2}},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        check_native_parity(c.code.data(), c.code.size(), seed_ymm_lanes,
                            false, true);
    }
}

// Memory source. Lane 1 addresses the SECOND 16 bytes of the 32-byte operand,
// which the driver implements by adding 16 to MemoryOperand::disp. A lowering
// that reuses lane 0's effective address reads the low 16 bytes twice.
TEST_F(JitParityTest, Native32BitVex256MemorySource) {
    constexpr GuestAddr kData = 0x3000;
    // 32 bytes whose two halves are unrelated, so reading the low half twice
    // is a visible divergence.
    // 48 bytes so the disp8 case below can still read a full 32.
    u8 operand[48];
    for (int i = 0; i < 48; ++i) {
        operand[i] = static_cast<u8>((i / 16) % 2 == 0 ? (0x11 * i + 7)
                                                       : (0xC3 ^ (i * 5 + 2)));
    }
    ASSERT_TRUE(m_memory->write_bytes(kData, operand, sizeof(operand)));

    auto setup = [](CpuState& cpu) {
        seed_ymm_lanes(cpu);
        cpu.gpr.set32(ECX, static_cast<u32>(kData));
    };

    struct Case { const char* name; std::vector<u8> code; };
    const std::vector<Case> cases = {
        // VPADDD ymm0, ymm1, [ecx]        -- mod=00, no displacement
        {"vpaddd ymm0,ymm1,[ecx]", {0xC5, 0xF5, 0xFE, 0x01}},
        // VPXOR  ymm0, ymm1, [ecx+0x10]   -- mod=01, disp8; the driver's +16
        // lands on top of an existing displacement
        {"vpxor ymm0,ymm1,[ecx+0x10]", {0xC5, 0xF5, 0xEF, 0x41, 0x10}},
        // VPSUBD ymm3, ymm1, [ecx]        -- non-commutative with a memory rm
        {"vpsubd ymm3,ymm1,[ecx]", {0xC5, 0xF5, 0xFA, 0x19}},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        check_native_parity(c.code.data(), c.code.size(), setup, false, true);
    }
}

// VEX.256 pure MOV forms in a 32-BIT block. VEX.256 is legal in compatibility
// mode and BoringSSL emits it, so this path is reachable in real guests.
//
// In a 64-bit block these are claimed by compile_64bit's dedicated 256-bit
// arms (via vex256_move_lowered_natively in ops64.cpp), so compile_sse never
// sees them. A 32-bit block has no such route: compile_sse is the only path,
// and the 128-bit MOV lowering calls emit_sse_reg_copy /
// emit_sse_load_mem128_fast, which take register INDICES and therefore always
// address xmm[] -- the LOW lane. If these opcodes are lane-split without a
// lane-aware move, pass 1 rewrites the low lane and ymm[255:128] is left
// STALE, silently.
TEST_F(JitParityTest, Native32BitVex256MovdquWritesBothLanes) {
    // c5 fe 6f c1 — vmovdqu ymm0, ymm1
    static const u8 kCode[] = {0xC5, 0xFE, 0x6F, 0xC1};

    auto setup = [](CpuState& cpu) {
        for (int i = 0; i < 4; ++i) {
            cpu.sse.xmm[0].u32_[i] = 0xDEADBEEFu;
            cpu.avx.ymm_high[0].u32_[i] = 0xDEADBEEFu;
            cpu.sse.xmm[1].u32_[i] = 0x11111111u;
            cpu.avx.ymm_high[1].u32_[i] = 0x22222222u;
        }
    };

    check_native_parity(kCode, sizeof(kCode), setup, /*check_flags=*/false,
                        /*check_xmm=*/true);

    // Pin the architectural answer, not just JIT/interpreter agreement.
    CpuState icpu;
    init_cpu(icpu);
    setup(icpu);
    ASSERT_TRUE(m_memory->write_bytes(0x1000, kCode, sizeof(kCode)).is_ok());
    {
        Interpreter interp(icpu, *m_memory);
        interp.run(1);
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(icpu.sse.xmm[0].u32_[i], 0x11111111u) << "low lane " << i;
        EXPECT_EQ(icpu.avx.ymm_high[0].u32_[i], 0x22222222u)
            << "high lane " << i << " was not written";
    }
}

TEST_F(JitParityTest, Vex256WordOperationsNativeBothLanesAndAliases) {
    for (u8 opcode : {u8{0x61}, u8{0x69}, u8{0xF5}}) {
        for (u8 dest : {u8{0}, u8{1}, u8{2}}) {
            check_native_parity({0xC5, 0xF5, opcode,
                                 static_cast<u8>(0xC2 | (dest << 3))},
                [](CpuState& cpu) {
                    for (int i = 0; i < 8; ++i) {
                        cpu.sse.xmm[1].u16_[i] = static_cast<u16>(0x8000 + i);
                        cpu.sse.xmm[2].u16_[i] = static_cast<u16>(0x1234 - i);
                        cpu.avx.ymm_high[1].u16_[i] = static_cast<u16>(0x5678 + i);
                        cpu.avx.ymm_high[2].u16_[i] = static_cast<u16>(0xFFFF - i);
                    }
                }, true, true);
        }
    }
}

// ---------------------------------------------------------------------------
// VEXTRACTF128 / VEXTRACTI128 in a 32-bit block (Intel SDM Vol. 2,
// VEX.256.66.0F3A.W0 19 / 39 /r ib): DEST[127:0] := imm8[0] ? SRC[255:128] :
// SRC[127:0], imm8[7:1] ignored; the register form zeroes DEST[MAXVL-1:128],
// the memory form is an unaligned 16-byte store. Expected bytes come from that
// text applied to the seeded input.
// ---------------------------------------------------------------------------
namespace {
u8 extract32_seed_byte(int reg, bool hi, int b) {
    const u8 lo = static_cast<u8>(0x10 * reg + b + 1);
    return hi ? static_cast<u8>(0xFF - lo) : lo;
}
void seed_extract32(CpuState& c) {
    for (int r = 0; r < 8; ++r) {
        for (int b = 0; b < 16; ++b) {
            c.sse.xmm[r].u8_[b] = extract32_seed_byte(r, false, b);
            c.avx.ymm_high[r].u8_[b] = extract32_seed_byte(r, true, b);
        }
    }
    c.flags.rflags = 0x202 | 0x1 | 0x4 | 0x40 | 0x80 | 0x800;
}
}  // namespace

TEST_F(JitParityTest, VextractRegisterDestinationEveryLaneNative) {
    struct Reg { u8 modrm; int dst; int src; };
    const Reg regs[] = {{0xC8, 0, 1}, {0xF7, 7, 6}, {0xDB, 3, 3}};
    for (u8 op : {u8{0x19}, u8{0x39}}) {
        for (const Reg& r : regs) {
            for (u8 imm : {u8{0x00}, u8{0x01}, u8{0xFE}, u8{0xFF}}) {
                SCOPED_TRACE(::testing::Message() << "op=" << std::hex << +op
                             << " dst=" << r.dst << " src=" << r.src << " imm=" << +imm);
                const std::vector<u8> code = {0xC4, 0xE3, 0x7D, op, r.modrm, imm};
                check_native_parity(code.data(), code.size(), seed_extract32, true, true);
                // Absolute check against the SDM on the JIT's own run.
                ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
                BasicBlock block = build_block(code.data(), code.size());
                JitCompiler jit(*m_memory);
                CompiledBlock* cb = jit.compile(block);
                ASSERT_NE(cb, nullptr);
                CpuState cpu;
                init_cpu(cpu);
                seed_extract32(cpu);
                test::execute_like_dispatcher(jit, *cb, cpu);
                for (int b = 0; b < 16; ++b) {
                    EXPECT_EQ(cpu.sse.xmm[r.dst].u8_[b], extract32_seed_byte(r.src, imm & 1, b));
                    EXPECT_EQ(cpu.avx.ymm_high[r.dst].u8_[b], 0u);
                }
            }
        }
    }
}

TEST_F(JitParityTest, VextractMemoryDestinationEveryAddressingFormNative) {
    struct Form {
        const char* name;
        int src;
        std::function<std::vector<u8>(u8 op, u8 imm, u32 ea)> code;
        std::function<void(CpuState&, u32 ea)> regs;
    };
    auto le32 = [](std::vector<u8>& v, u32 x) {
        for (int i = 0; i < 4; ++i) v.push_back(static_cast<u8>(x >> (8 * i)));
    };
    const std::vector<Form> forms = {
        {"[eax]", 1,
         [](u8 op, u8 imm, u32) { return std::vector<u8>{0xC4, 0xE3, 0x7D, op, 0x08, imm}; },
         [](CpuState& c, u32 ea) { c.gpr.set32(EAX, ea); }},
        {"[eax+ecx*4+0x10]", 2,
         [](u8 op, u8 imm, u32) {
             return std::vector<u8>{0xC4, 0xE3, 0x7D, op, 0x54, 0x88, 0x10, imm};
         },
         [](CpuState& c, u32 ea) { c.gpr.set32(ECX, 3); c.gpr.set32(EAX, ea - 0x1C); }},
        {"[disp32]", 3,
         [le32](u8 op, u8 imm, u32 ea) {
             std::vector<u8> v{0xC4, 0xE3, 0x7D, op, 0x1D};
             le32(v, ea);
             v.push_back(imm);
             return v;
         },
         [](CpuState&, u32) {}},
        {"[esi+disp32] wraps 2^32", 6,
         [le32](u8 op, u8 imm, u32) {
             std::vector<u8> v{0xC4, 0xE3, 0x7D, op, 0xB6};
             le32(v, 0x80000000u);
             v.push_back(imm);
             return v;
         },
         [](CpuState& c, u32 ea) { c.gpr.set32(ESI, ea + 0x80000000u); }},
        {"[ebp+8], ymm7", 7,
         [](u8 op, u8 imm, u32) { return std::vector<u8>{0xC4, 0xE3, 0x7D, op, 0x7D, 0x08, imm}; },
         [](CpuState& c, u32 ea) { c.gpr.set32(EBP, ea - 8); }},
        {"fs:[ebx]", 5,
         [](u8 op, u8 imm, u32) {
             return std::vector<u8>{0x64, 0xC4, 0xE3, 0x7D, op, 0x2B, imm};
         },
         [](CpuState& c, u32 ea) { c.seg.fs_desc.base = 0x7000; c.gpr.set32(EBX, ea - 0x7000); }},
        {"cmp; [eax] (live flags)", 1,
         [](u8 op, u8 imm, u32) {
             return std::vector<u8>{0x39, 0xC1, 0xC4, 0xE3, 0x7D, op, 0x08, imm};
         },
         [](CpuState& c, u32 ea) { c.gpr.set32(EAX, ea); c.gpr.set32(ECX, 5); }},
    };
    const u32 eas[] = {0x8000, 0x8005, 0x8FF8, 0xBFF9};
    for (const Form& f : forms) {
        for (u8 op : {u8{0x19}, u8{0x39}}) {
            for (u8 imm : {u8{0x00}, u8{0x01}, u8{0xFE}, u8{0xFF}}) {
                for (u32 ea : eas) {
                    SCOPED_TRACE(::testing::Message() << f.name << " op=" << std::hex << +op
                                 << " imm=" << +imm << " ea=" << ea);
                    std::vector<u8> fill(48);
                    for (usize i = 0; i < fill.size(); ++i) fill[i] = static_cast<u8>(0xC0 ^ i);
                    ASSERT_TRUE(m_memory->write_bytes(ea - 16, fill.data(), fill.size()));
                    const auto code = f.code(op, imm, ea);
                    auto setup = [&](CpuState& c) { seed_extract32(c); f.regs(c, ea); };
                    // check_parity restores memory and compares the full
                    // mapping; the JIT's result is what remains afterwards.
                    check_native_parity(code.data(), code.size(), setup, true, true);
                    std::vector<u8> got(48);
                    ASSERT_TRUE(m_memory->read_bytes(ea - 16, got.data(), got.size()));
                    for (int b = 0; b < 16; ++b) {
                        EXPECT_EQ(got[16 + b], extract32_seed_byte(f.src, imm & 1, b)) << "byte " << b;
                        EXPECT_EQ(got[b], fill[b]) << "below the store";
                        EXPECT_EQ(got[32 + b], fill[32 + b]) << "above the store";
                    }
                }
            }
        }
    }
}

TEST_F(JitParityTest, VextractMemoryFaultStoresNothingNative) {
    ScopedEnvVar nd("VITREO_JIT_NO_DELEGATE", "1");
    for (u8 op : {u8{0x19}, u8{0x39}}) {
        for (u8 imm : {u8{0x00}, u8{0x01}}) {
            const std::vector<u8> code = {0x39, 0xC1, 0xC4, 0xE3, 0x7D, op, 0x08, imm};
            ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
            BasicBlock block = build_block(code.data(), code.size());
            ASSERT_EQ(block.instruction_count, 2u);
            JitCompiler jit(*m_memory);
            CompiledBlock* compiled = jit.compile(block);
            ASSERT_NE(compiled, nullptr);
            for (u32 writable : {0u, 1u, 8u, 15u}) {
                SCOPED_TRACE(::testing::Message() << "op=" << std::hex << +op
                             << " imm=" << +imm << " writable=" << std::dec << writable);
                const u32 ea = 0x11000 - writable;   // mapping ends at 0x11000
                std::vector<u8> before(16, 0x77);
                if (writable) ASSERT_TRUE(m_memory->write_bytes(ea, before.data(), writable));
                CpuState cpu;
                init_cpu(cpu);
                seed_extract32(cpu);
                cpu.gpr.set32(EAX, ea);
                cpu.gpr.set32(ECX, 5);

                CpuState interpreted = cpu;
                {
                    Interpreter interpreter(interpreted, *m_memory);
                    EXPECT_EQ(interpreter.step(), ExecResult::Continue);
                    EXPECT_EQ(interpreter.step(), ExecResult::Exception);
                }
                std::vector<u8> after(16, 0);
                if (writable) {
                    ASSERT_TRUE(m_memory->read_bytes(ea, after.data(), writable));
                    for (u32 i = 0; i < writable; ++i) EXPECT_EQ(after[i], 0x77u) << "interp byte " << i;
                }

                EXPECT_EQ(test::execute_like_dispatcher(jit, *compiled, cpu),
                          JitExecResult::Continue);
                ASSERT_TRUE(cpu.pending_raise.active);
                EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
                EXPECT_EQ(cpu.pending_raise.args[0], 1u);          // write
                EXPECT_EQ(cpu.pending_raise.args[1], 0x11000u);    // first denied byte
                EXPECT_EQ(static_cast<u32>(cpu.rip), 0x1002u);     // the VEXTRACT
                if (writable) {
                    ASSERT_TRUE(m_memory->read_bytes(ea, after.data(), writable));
                    for (u32 i = 0; i < writable; ++i) EXPECT_EQ(after[i], 0x77u) << "jit byte " << i;
                }
                cpu.materialize_lazy_flags();
                interpreted.materialize_lazy_flags();
                EXPECT_EQ(cpu.flags.cf(), interpreted.flags.cf());
                EXPECT_EQ(cpu.flags.zf(), interpreted.flags.zf());
                EXPECT_EQ(cpu.flags.sf(), interpreted.flags.sf());
                EXPECT_EQ(cpu.flags.of(), interpreted.flags.of());
                EXPECT_TRUE(cpu.flags.cf());                       // 5 < ea, unsigned
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Faulting 16- and 32-byte vector stores in a 32-bit block: interpreter vs
// JIT and both against Windows (simd_store_fault_cases.hpp). Boundaries: the
// end of the code region, a separate data region, and a read-only page.
// ---------------------------------------------------------------------------
#include "simd_store_fault_cases.hpp"

namespace {
vitreo::cpu::test_simd_store::Env simd_store_env32(
    GuestMemory& mem, std::function<BasicBlock(const u8*, usize)> build,
    std::function<void(CpuState&)> init) {
    return {mem, 0x1000, false,
            [build](const std::vector<u8>& code) { return build(code.data(), code.size()); },
            init,
            [&mem](const BasicBlock& block, CpuState& cpu) {
                JitCompiler jit(mem);
                CompiledBlock* cb = jit.compile(block);
                if (cb == nullptr) {
                    ADD_FAILURE() << "store block did not compile natively";
                    return JitExecResult::StaleCode;
                }
                return test::execute_like_dispatcher(jit, *cb, cpu);
            }};
}
}  // namespace

TEST_F(JitParityTest, VectorStoreFaultsMatchInterpreterAndWindows) {
    ScopedEnvVar nd("VITREO_JIT_NO_DELEGATE", "1");
    ASSERT_TRUE(m_memory->map_region(0x20000, 0x2000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->map_region(0x30000, 0x1000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->map_region(0x31000, 0x1000, MemoryProtection::Read));
    const auto env = simd_store_env32(
        *m_memory, [this](const u8* c, usize n) { return build_block(c, n); },
        [this](CpuState& c) { init_cpu(c); });
    for (GuestAddr b : {GuestAddr{0x11000}, GuestAddr{0x22000}, GuestAddr{0x31000}}) {
        SCOPED_TRACE(::testing::Message() << "boundary 0x" << std::hex << b);
        vitreo::cpu::test_simd_store::run_all_forms(env, b);
    }
}
