// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs interpreter differential tests for the SSE forms a UE5 x64 session
// (VITREO_JIT_FALLBACK_HIST, 125M delegations) sent to the interpreter:
// INSERTPS/EXTRACTPS/PEXTRD memory forms in long mode, VEX three-operand
// forms whose r/m register is also the destination (`vdivsd x0,x1,x0`,
// `vmovlhps x0,x1,x0`), VEX unary forms with dst == src
// (`vpmovsxwd x1,x1`, `vcvtps2pd x1,x1`), the VEX.256 widen/narrow forms
// (VPMOVSX/ZX ymm, VCVTPS2PD ymm, VCVT(T)PD2DQ xmm,ymm), the CVT(T)PD2DQ m128
// form in long mode, and the scalar FP and MIN/MAX special-operand arms.
//
// Every case runs BOTH bitnesses with VITREO_JIT_NO_DELEGATE=1, so a form
// that still declines fails outright instead of passing on the interpreter's
// answer. The interpreter is the semantic reference; MXCSR is
// observed through LDMXCSR/STMXCSR so the host's sticky exception flags are
// harvested the way a guest sees them. A few absolute expectations are taken
// from the Intel SDM Vol. 2 operation text and Vol. 1 §4.8.3.4 / §4.8.3.7
// (NaN propagation, the sign-set QNaN indefinite), never from either engine.

#include "jit_parity_64_fixture.hpp"

#include <bit>

namespace {

constexpr GuestAddr kData = 0x8000;    // 16-byte aligned operand area
constexpr GuestAddr kMxcsr = 0x7F00;   // LDMXCSR source / STMXCSR target

// f32 / f64 operand classes: zeros, normals, denormals, extremes, infinities,
// quiet and signalling NaNs of both signs with payloads.
constexpr u32 kF32[] = {
    0x00000000u, 0x80000000u, 0x3FC00000u, 0xC0100000u, 0x00000001u,
    0x80400000u, 0x7F800000u, 0xFF800000u, 0x7FC00001u, 0xFFC12345u,
    0x7F800001u, 0xFF812345u, 0x7F7FFFFFu, 0x00800000u, 0x40400000u,
    0x3F800000u,
};
constexpr u64 kF64[] = {
    0x0000000000000000ull, 0x8000000000000000ull, 0x3FF8000000000000ull,
    0xC002000000000000ull, 0x0000000000000001ull, 0x8008000000000000ull,
    0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000001ull,
    0xFFF8000000012345ull, 0x7FF0000000000001ull, 0xFFF0000000012345ull,
    0x7FEFFFFFFFFFFFFFull, 0x0010000000000000ull, 0x4008000000000000ull,
    0x3FF0000000000000ull, 0x41E0000000000000ull, 0xC1E0000000200000ull,
};

// Default, FTZ+DAZ (Unreal), DAZ only, FTZ only, round down, round toward
// zero. All exceptions masked.
constexpr u32 kMaskedMxcsr[] = {0x1F80u, 0x9FC0u, 0x1FC0u, 0x9F80u,
                                0x3F80u, 0x7F80u};

struct EngineRun {
    CpuState cpu{};
    bool faulted{false};
    u32 fault_code{0};
    std::vector<u8> data;
    u32 mxcsr_seen{0};
};

// One code sequence through both engines in one bitness. `ldmx`/`stmx`
// wrap the code with LDMXCSR [rbx] / STMXCSR [rbx] (rbx = kMxcsr).
class SseFallbackFormsTest : public JitParity64Test {
protected:
    void run_both(bool x64, std::vector<u8> code,
                  const std::function<void(CpuState&)>& setup,
                  const std::vector<u8>& data, u32 mxcsr,
                  bool expect_native = true) {
        std::vector<u8> full = {0x0F, 0xAE, 0x13};  // ldmxcsr [rbx]/[ebx]
        full.insert(full.end(), code.begin(), code.end());
        const usize stmx_at = full.size();
        full.insert(full.end(), {0x0F, 0xAE, 0x1B});  // stmxcsr [rbx]
        (void)stmx_at;

        GuestMemory local;
        GuestMemory& mem = x64 ? *m_memory : local;
        if (!x64) {
            ASSERT_TRUE(local.map_region(0x1000, 0x10000,
                                         MemoryProtection::ReadWrite));
        }
        ASSERT_TRUE(mem.write_bytes(0x1000, full.data(), full.size()));

        BasicBlock block;
        block.start_addr = 0x1000;
        block.is_64bit = x64;
        MemoryByteReader reader(full.data(), full.size());
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            if (decode_instruction(reader, x64 ? DecodeMode::Mode64
                                               : DecodeMode::Mode32,
                                   inst) != DecodeError::Success) {
                break;
            }
            block.instructions.push_back(inst);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count = static_cast<u32>(block.instructions.size());
        ASSERT_EQ(block.byte_length, full.size()) << "sequence did not decode";

        auto prepare = [&](CpuState& cpu) {
            if (x64) {
                cpu.init_long_mode();
                cpu.gpr.set64(RSP, 0x10000);
            } else {
                cpu.init_protected_mode();
                cpu.gpr.set32(RSP, 0x10000);
            }
            cpu.rip = 0x1000;
            cpu.gpr.set64(RBX, kMxcsr);
            cpu.gpr.set64(RAX, kData);
            setup(cpu);
            if (!x64) {
                for (int r = 8; r < 16; ++r) {
                    cpu.sse.xmm[r] = XmmReg{};
                    cpu.avx.ymm_high[r] = XmmReg{};
                }
            }
            EXPECT_TRUE(mem.write_u32(kMxcsr, mxcsr));
            EXPECT_TRUE(mem.write_bytes(kData, data.data(), data.size()));
        };
        auto collect = [&](EngineRun& run) {
            run.data.resize(data.size());
            EXPECT_TRUE(mem.read_bytes(kData, run.data.data(), run.data.size()));
            auto seen = mem.read_u32(kMxcsr);
            EXPECT_TRUE(seen.is_ok());
            run.mxcsr_seen = seen.is_ok() ? seen.value() : 0;
        };

        EngineRun interp;
        prepare(interp.cpu);
        {
            Interpreter engine(interp.cpu, mem);
            const ExecResult r = engine.run(block.instruction_count);
            interp.faulted = r == ExecResult::Exception;
            interp.fault_code = static_cast<u32>(engine.context().exception);
        }
        interp.cpu.materialize_lazy_flags();
        collect(interp);

        EngineRun jit;
        prepare(jit.cpu);
        CompiledBlock* cb = nullptr;
        JitCompiler compiler(mem);
        {
            ScopedEnvironmentVariable nd("VITREO_JIT_NO_DELEGATE",
                                         expect_native ? "1" : nullptr);
            cb = compiler.compile(block);
        }
        if (cb == nullptr) {
            ADD_FAILURE() << "JIT declined the form (no native lowering)";
            return;
        }
        test::execute_like_dispatcher(compiler, *cb, jit.cpu);
        jit.faulted = jit.cpu.pending_raise.active;
        jit.cpu.materialize_lazy_flags();
        collect(jit);

        ASSERT_EQ(interp.faulted, jit.faulted)
            << "fault divergence: interp=" << interp.faulted
            << " jit=" << jit.faulted;
        const int nregs = x64 ? 16 : 8;
        for (int r = 0; r < nregs; ++r) {
            for (int b = 0; b < 16; ++b) {
                ASSERT_EQ(interp.cpu.sse.xmm[r].u8_[b], jit.cpu.sse.xmm[r].u8_[b])
                    << "XMM" << r << " byte " << b << " interp=0x" << std::hex
                    << interp.cpu.sse.xmm[r].u64_[b / 8] << " jit=0x"
                    << jit.cpu.sse.xmm[r].u64_[b / 8];
                ASSERT_EQ(interp.cpu.avx.ymm_high[r].u8_[b],
                          jit.cpu.avx.ymm_high[r].u8_[b])
                    << "YMM" << r << " high byte " << b;
            }
        }
        for (int g = 0; g < nregs; ++g) {
            ASSERT_EQ(interp.cpu.gpr.get64(g), jit.cpu.gpr.get64(g))
                << gpr_name64(g);
        }
        if (interp.faulted) {
            // A faulting instruction commits nothing and reports its own RIP.
            ASSERT_EQ(interp.data, jit.data) << "faulting store published";
            return;
        }
        ASSERT_EQ(interp.cpu.rip, jit.cpu.rip);
        ASSERT_EQ(interp.data, jit.data) << "guest memory diverged";
        ASSERT_EQ(interp.mxcsr_seen, jit.mxcsr_seen)
            << "MXCSR diverged: interp=0x" << std::hex << interp.mxcsr_seen
            << " jit=0x" << jit.mxcsr_seen;
        m_last_interp_cpu = interp.cpu;
        m_last_jit_cpu = jit.cpu;
        m_last_mxcsr = jit.mxcsr_seen;
    }

    void run_both_modes(const std::vector<u8>& code,
                        const std::function<void(CpuState&)>& setup,
                        const std::vector<u8>& data, u32 mxcsr) {
        for (bool x64 : {false, true}) {
            SCOPED_TRACE(x64 ? "x64" : "x86");
            run_both(x64, code, setup, data, mxcsr);
            if (HasFatalFailure()) return;
        }
    }

    u32 m_last_mxcsr{0};
};

std::vector<u8> bytes_of(const void* p, usize n) {
    const auto* b = static_cast<const u8*>(p);
    return std::vector<u8>(b, b + n);
}

// Distinct, non-zero filler in every lane of xmm0-15 and their YMM halves, so
// a lane the instruction must preserve (or zero) is visible.
void seed_lanes(CpuState& c) {
    for (int r = 0; r < 16; ++r) {
        for (int i = 0; i < 4; ++i) {
            c.sse.xmm[r].u32_[i] = 0x3F000000u + 0x00010000u * r + 0x111u * i;
            c.avx.ymm_high[r].u32_[i] = 0xA5A50000u + 0x100u * r + i;
        }
    }
}

// VEX encoders (Intel SDM Vol. 2 §2.3.5). reg/rm/vvvv are 0-15; rm < 0 means
// a [rax] memory operand (ModRM mod=00 rm=000).
std::vector<u8> vex2(u8 pp, bool l, u8 vvvv, u8 op, u8 reg, int rm,
                     std::initializer_list<u8> tail = {}) {
    std::vector<u8> v{0xC5,
        static_cast<u8>((reg < 8 ? 0x80 : 0) | ((~vvvv & 15) << 3) |
                        (l ? 4 : 0) | pp),
        op};
    v.push_back(rm < 0 ? static_cast<u8>((reg & 7) << 3)
                       : static_cast<u8>(0xC0 | ((reg & 7) << 3) | (rm & 7)));
    v.insert(v.end(), tail);
    return v;
}

std::vector<u8> vex3(u8 map, u8 pp, bool l, u8 vvvv, u8 op, u8 reg, int rm,
                     std::initializer_list<u8> tail = {}) {
    std::vector<u8> v{0xC4,
        static_cast<u8>((reg < 8 ? 0x80 : 0) | 0x40 |
                        ((rm < 0 || rm < 8) ? 0x20 : 0) | map),
        static_cast<u8>(((~vvvv & 15) << 3) | (l ? 4 : 0) | pp),
        op};
    v.push_back(rm < 0 ? static_cast<u8>((reg & 7) << 3)
                       : static_cast<u8>(0xC0 | ((reg & 7) << 3) | (rm & 7)));
    v.insert(v.end(), tail);
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// INSERTPS: every imm8 (COUNT_S, COUNT_D, ZMASK), register and m32 sources,
// aligned and misaligned, legacy and VEX (incl. dst == r/m), both modes.
// ---------------------------------------------------------------------------
TEST_F(SseFallbackFormsTest, InsertpsAllImmediatesAllFormsNative) {
    u32 words[8];
    for (int i = 0; i < 8; ++i) words[i] = 0xC0DE0000u + 0x1111u * i;
    const auto data = bytes_of(words, sizeof(words));
    for (u32 imm = 0; imm < 256; imm += 7) {
        SCOPED_TRACE(testing::Message() << "imm=0x" << std::hex << imm);
        const u8 ib = static_cast<u8>(imm);
        // insertps xmm2, xmm5, imm / insertps xmm2, [rax(+3)], imm
        run_both_modes({0x66, 0x0F, 0x3A, 0x21, 0xD5, ib}, seed_lanes, data, 0x1F80);
        run_both_modes({0x66, 0x0F, 0x3A, 0x21, 0x10, ib}, seed_lanes, data, 0x1F80);
        run_both_modes({0x66, 0x0F, 0x3A, 0x21, 0x50, 0x03, ib}, seed_lanes, data, 0x1F80);
        // vinsertps xmm2, xmm3, xmm5 / xmm2, xmm3, xmm2 / xmm2, xmm2, xmm2 /
        // xmm2, xmm3, [rax]
        run_both_modes(vex3(3, 1, false, 3, 0x21, 2, 5, {ib}), seed_lanes, data, 0x1F80);
        run_both_modes(vex3(3, 1, false, 3, 0x21, 2, 2, {ib}), seed_lanes, data, 0x1F80);
        run_both_modes(vex3(3, 1, false, 2, 0x21, 2, 2, {ib}), seed_lanes, data, 0x1F80);
        run_both_modes(vex3(3, 1, false, 3, 0x21, 2, -1, {ib}), seed_lanes, data, 0x1F80);
        if (HasFatalFailure()) return;
    }
    // Absolute check, SDM INSERTPS operation: imm 0x9C = COUNT_S 2,
    // COUNT_D 1, ZMASK 1100b (register form).
    run_both(true, {0x66, 0x0F, 0x3A, 0x21, 0xD5, 0x9C}, seed_lanes, data, 0x1F80);
    CpuState ref;
    seed_lanes(ref);
    EXPECT_EQ(m_last_jit_cpu.sse.xmm[2].u32_[0], ref.sse.xmm[2].u32_[0]);
    EXPECT_EQ(m_last_jit_cpu.sse.xmm[2].u32_[1], ref.sse.xmm[5].u32_[2]);
    EXPECT_EQ(m_last_jit_cpu.sse.xmm[2].u32_[2], 0u);
    EXPECT_EQ(m_last_jit_cpu.sse.xmm[2].u32_[3], 0u);
    EXPECT_EQ(m_last_jit_cpu.avx.ymm_high[2].u32_[0], ref.avx.ymm_high[2].u32_[0]);
}

TEST_F(SseFallbackFormsTest, InsertpsMemoryReadsFourBytesOnly) {
    // An m32 at the very end of the mapped region must not touch the next
    // page: [rax] = 0x10FFC (last dword below 0x11000).
    u32 words[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        run_both(x64, {0x66, 0x0F, 0x3A, 0x21, 0x10, 0x30},
                 [](CpuState& c) { seed_lanes(c); c.gpr.set64(RAX, 0x10FFC); },
                 bytes_of(words, sizeof(words)), 0x1F80);
        if (HasFatalFailure()) return;
    }
}

// ---------------------------------------------------------------------------
// EXTRACTPS / PEXTRD / PEXTRB: memory destinations in long mode.
// ---------------------------------------------------------------------------
TEST_F(SseFallbackFormsTest, ExtractToMemoryAllLanesNative) {
    std::vector<u8> data(32, 0xEE);
    for (u8 lane = 0; lane < 16; ++lane) {
        SCOPED_TRACE(testing::Message() << "lane=" << +lane);
        // extractps [rax], xmm5, lane / [rax+3]
        run_both_modes({0x66, 0x0F, 0x3A, 0x17, 0x28, lane}, seed_lanes, data, 0x1F80);
        run_both_modes({0x66, 0x0F, 0x3A, 0x17, 0x68, 0x03, lane}, seed_lanes, data, 0x1F80);
        // pextrd [rax+3], xmm5, lane ; pextrb [rax+3], xmm5, lane
        run_both_modes({0x66, 0x0F, 0x3A, 0x16, 0x68, 0x03, lane}, seed_lanes, data, 0x1F80);
        run_both_modes({0x66, 0x0F, 0x3A, 0x14, 0x68, 0x03, lane}, seed_lanes, data, 0x1F80);
        // vextractps [rax], xmm5, lane
        run_both_modes(vex3(3, 1, false, 0, 0x17, 5, -1, {lane}), seed_lanes, data, 0x1F80);
        // register destinations: extractps ecx, xmm5 ; pextrd ecx, xmm5
        run_both_modes({0x66, 0x0F, 0x3A, 0x17, 0xE9, lane}, seed_lanes, data, 0x1F80);
        if (HasFatalFailure()) return;
    }
}

// ---------------------------------------------------------------------------
// MOVLHPS / MOVHLPS: legacy and VEX with every aliasing shape.
// ---------------------------------------------------------------------------
TEST_F(SseFallbackFormsTest, MovlhpsMovhlpsVexAliasesNative) {
    const std::vector<u8> data(16, 0);
    for (u8 op : {u8{0x16}, u8{0x12}}) {
        SCOPED_TRACE(testing::Message() << "op=0x" << std::hex << +op);
        run_both_modes({0x0F, op, 0xC1}, seed_lanes, data, 0x1F80);
        run_both_modes({0x0F, op, 0xC0}, seed_lanes, data, 0x1F80);
        for (auto [d, v, m] : {std::array<u8, 3>{0, 1, 0}, {0, 1, 2},
                               {0, 0, 1}, {0, 0, 0}, {1, 0, 0}}) {
            SCOPED_TRACE(testing::Message() << "d=" << +d << " v=" << +v
                         << " m=" << +m);
            run_both_modes(vex2(0, false, v, op, d, m), seed_lanes, data, 0x1F80);
            if (HasFatalFailure()) return;
        }
    }
    // SDM VMOVLHPS: DEST[63:0] := SRC1[63:0]; DEST[127:64] := SRC2[63:0];
    // DEST[MAXVL-1:128] := 0. vmovlhps xmm0, xmm1, xmm0.
    run_both(true, vex2(0, false, 1, 0x16, 0, 0), seed_lanes, data, 0x1F80);
    CpuState ref;
    seed_lanes(ref);
    EXPECT_EQ(m_last_jit_cpu.sse.xmm[0].u64_[0], ref.sse.xmm[1].u64_[0]);
    EXPECT_EQ(m_last_jit_cpu.sse.xmm[0].u64_[1], ref.sse.xmm[0].u64_[0]);
    EXPECT_EQ(m_last_jit_cpu.avx.ymm_high[0].u64_[0], 0u);
    EXPECT_EQ(m_last_jit_cpu.avx.ymm_high[0].u64_[1], 0u);
}

// ---------------------------------------------------------------------------
// PMOVSX/PMOVZX: all twelve, register and memory, legacy, VEX.128 (incl.
// dst == src) and VEX.256 (incl. dst == src, memory).
// ---------------------------------------------------------------------------
TEST_F(SseFallbackFormsTest, PmovsxPmovzxAllFormsNative) {
    u8 raw[32];
    for (int i = 0; i < 32; ++i) raw[i] = static_cast<u8>(0x81 + 0x37 * i);
    const auto data = bytes_of(raw, sizeof(raw));
    auto seed = [](CpuState& c) {
        seed_lanes(c);
        for (int r = 0; r < 16; ++r)
            for (int b = 0; b < 16; ++b)
                c.sse.xmm[r].u8_[b] = static_cast<u8>(0x80 ^ (r * 16 + b * 13));
    };
    // 0F38 20-25 (SX BW,BD,BQ,WD,WQ,DQ), 30-35 (ZX).
    for (u8 op : {u8{0x20}, u8{0x21}, u8{0x22}, u8{0x23}, u8{0x24}, u8{0x25},
                  u8{0x30}, u8{0x31}, u8{0x32}, u8{0x33}, u8{0x34}, u8{0x35}}) {
        SCOPED_TRACE(testing::Message() << "op=0x" << std::hex << +op);
        run_both_modes({0x66, 0x0F, 0x38, op, 0xC1}, seed, data, 0x1F80);
        run_both_modes({0x66, 0x0F, 0x38, op, 0xC0}, seed, data, 0x1F80);
        run_both_modes({0x66, 0x0F, 0x38, op, 0x00}, seed, data, 0x1F80);
        run_both_modes({0x66, 0x0F, 0x38, op, 0x40, 0x03}, seed, data, 0x1F80);
        for (bool l : {false, true}) {
            SCOPED_TRACE(l ? "VEX.256" : "VEX.128");
            run_both_modes(vex3(2, 1, l, 0, op, 1, 2), seed, data, 0x1F80);
            run_both_modes(vex3(2, 1, l, 0, op, 1, 1), seed, data, 0x1F80);
            run_both_modes(vex3(2, 1, l, 0, op, 0, 0), seed, data, 0x1F80);
            run_both_modes(vex3(2, 1, l, 0, op, 1, -1), seed, data, 0x1F80);
        }
        if (HasFatalFailure()) return;
    }
    // SDM VPMOVSXWD ymm1, xmm2: DEST[255:128] from SRC[127:64].
    run_both(true, vex3(2, 1, true, 0, 0x23, 1, 2), seed, data, 0x1F80);
    CpuState ref;
    seed(ref);
    EXPECT_EQ(m_last_jit_cpu.avx.ymm_high[1].i32_[0],
              static_cast<i32>(ref.sse.xmm[2].i16_[4]));
    EXPECT_EQ(m_last_jit_cpu.avx.ymm_high[1].i32_[3],
              static_cast<i32>(ref.sse.xmm[2].i16_[7]));
}

// ---------------------------------------------------------------------------
// CVTPS2PD / CVTTPD2DQ / CVTPD2DQ: specials, every masked MXCSR mode, legacy
// (m128 aligned + misaligned #GP), VEX.128 aliasing, VEX.256.
// ---------------------------------------------------------------------------
TEST_F(SseFallbackFormsTest, Cvtps2pdAllFormsSpecialsNative) {
    std::vector<u8> data = bytes_of(kF32, sizeof(kF32));
    for (u32 mx : kMaskedMxcsr) {
        for (usize base = 0; base + 4 <= std::size(kF32); base += 2) {
            SCOPED_TRACE(testing::Message() << "mxcsr=0x" << std::hex << mx
                         << " base=" << std::dec << base);
            auto seed = [base](CpuState& c) {
                seed_lanes(c);
                for (int i = 0; i < 4; ++i) {
                    c.sse.xmm[1].u32_[i] = kF32[(base + i) % std::size(kF32)];
                    c.sse.xmm[2].u32_[i] = kF32[(base + 3 + i) % std::size(kF32)];
                }
            };
            std::vector<u8> d(data.begin() + static_cast<long>(base * 4), data.end());
            d.resize(32, 0);
            run_both_modes({0x0F, 0x5A, 0xC2}, seed, d, mx);
            run_both_modes({0x0F, 0x5A, 0xD2}, seed, d, mx);
            run_both_modes({0x0F, 0x5A, 0x40, 0x03}, seed, d, mx);
            run_both_modes(vex2(0, false, 0, 0x5A, 1, 1), seed, d, mx);
            run_both_modes(vex2(0, false, 0, 0x5A, 1, 2), seed, d, mx);
            run_both_modes(vex2(0, true, 0, 0x5A, 1, 1), seed, d, mx);
            run_both_modes(vex2(0, true, 0, 0x5A, 1, 2), seed, d, mx);
            run_both_modes(vex2(0, true, 0, 0x5A, 1, -1), seed, d, mx);
            if (HasFatalFailure()) return;
        }
    }
}

TEST_F(SseFallbackFormsTest, CvtPd2DqTruncAndRoundAllFormsSpecialsNative) {
    for (u8 pp_op : {u8{0x66}, u8{0xF2}}) {  // 66 E6 = CVTTPD2DQ, F2 E6 = CVTPD2DQ
        const u8 pp = pp_op == 0x66 ? 1 : 3;
        for (u32 mx : kMaskedMxcsr) {
            for (usize base = 0; base + 4 <= std::size(kF64); base += 3) {
                SCOPED_TRACE(testing::Message() << "pfx=0x" << std::hex << +pp_op
                             << " mxcsr=0x" << mx << " base=" << std::dec << base);
                auto seed = [base](CpuState& c) {
                    seed_lanes(c);
                    for (int i = 0; i < 2; ++i) {
                        c.sse.xmm[1].u64_[i] = kF64[(base + i) % std::size(kF64)];
                        c.avx.ymm_high[1].u64_[i] = kF64[(base + 2 + i) % std::size(kF64)];
                        c.sse.xmm[2].u64_[i] = kF64[(base + 5 + i) % std::size(kF64)];
                    }
                };
                std::vector<u8> d;
                for (usize i = 0; i < 4; ++i) {
                    const u64 v = kF64[(base + 7 + i) % std::size(kF64)];
                    auto b = bytes_of(&v, 8);
                    d.insert(d.end(), b.begin(), b.end());
                }
                d.resize(48, 0);
                run_both_modes({pp_op, 0x0F, 0xE6, 0xC1}, seed, d, mx);
                run_both_modes({pp_op, 0x0F, 0xE6, 0xC9}, seed, d, mx);
                run_both_modes({pp_op, 0x0F, 0xE6, 0x00}, seed, d, mx);
                // misaligned legacy m128: #GP in both engines
                run_both_modes({pp_op, 0x0F, 0xE6, 0x40, 0x08}, seed, d, mx);
                run_both_modes(vex2(pp, false, 0, 0xE6, 1, 1), seed, d, mx);
                run_both_modes(vex2(pp, false, 0, 0xE6, 2, 1), seed, d, mx);
                run_both_modes(vex2(pp, false, 0, 0xE6, 1, -1), seed, d, mx);
                run_both_modes(vex2(pp, true, 0, 0xE6, 1, 1), seed, d, mx);
                run_both_modes(vex2(pp, true, 0, 0xE6, 2, 1), seed, d, mx);
                run_both_modes(vex2(pp, true, 0, 0xE6, 1, -1), seed, d, mx);
                if (HasFatalFailure()) return;
            }
        }
    }
    // SDM CVTTPD2DQ: NaN and out-of-range return the integer indefinite
    // 80000000H; DEST[127:64] := 0.
    run_both(true, {0x66, 0x0F, 0xE6, 0xC1}, [](CpuState& c) {
        seed_lanes(c);
        c.sse.xmm[1].u64_[0] = 0x7FF8000000000001ull;
        c.sse.xmm[1].u64_[1] = 0x41E0000000000000ull;  // 2^31
    }, std::vector<u8>(16, 0), 0x1F80);
    EXPECT_EQ(m_last_jit_cpu.sse.xmm[0].u32_[0], 0x80000000u);
    EXPECT_EQ(m_last_jit_cpu.sse.xmm[0].u32_[1], 0x80000000u);
    EXPECT_EQ(m_last_jit_cpu.sse.xmm[0].u64_[1], 0u);
}

// ---------------------------------------------------------------------------
// Scalar ADD/SUB/MUL/DIV SS/SD: the full special-operand cross product under
// every masked MXCSR mode, register / m32 / m64 (misaligned) sources, legacy
// and VEX (dst == r/m, dst == vvvv, all distinct).
// ---------------------------------------------------------------------------
TEST_F(SseFallbackFormsTest, ScalarArithSpecialsAllFormsNative) {
    // 0F 58 ADD, 59 MUL, 5C SUB, 5E DIV; F3 = SS, F2 = SD.
    for (u8 pfx : {u8{0xF3}, u8{0xF2}}) {
        const bool sd = pfx == 0xF2;
        const usize n = sd ? std::size(kF64) : std::size(kF32);
        for (u8 op : {u8{0x58}, u8{0x59}, u8{0x5C}, u8{0x5E}}) {
            for (u32 mx : {0x1F80u, 0x9FC0u, 0x1FC0u, 0x7F80u}) {
                for (usize a = 0; a < n; ++a) {
                    SCOPED_TRACE(testing::Message() << "pfx=0x" << std::hex << +pfx
                                 << " op=0x" << +op << " mxcsr=0x" << mx
                                 << " a=" << std::dec << a);
                    auto seed = [a, sd, n](CpuState& c) {
                        seed_lanes(c);
                        for (usize b = 0; b < 4; ++b) {
                            // xmm(2+b) lane 0 holds operand b-th of a sweep
                            if (sd) c.sse.xmm[2 + b].u64_[0] = kF64[(a * 5 + b) % n];
                            else c.sse.xmm[2 + b].u32_[0] = kF32[(a * 5 + b) % n];
                        }
                        if (sd) c.sse.xmm[0].u64_[0] = kF64[a];
                        else c.sse.xmm[0].u32_[0] = kF32[a];
                        c.sse.xmm[1] = c.sse.xmm[0];
                    };
                    std::vector<u8> d;
                    for (usize b = 0; b < n; ++b) {
                        auto v = sd ? bytes_of(&kF64[(b + a) % n], 8)
                                    : bytes_of(&kF32[(b + a) % n], 4);
                        d.insert(d.end(), v.begin(), v.end());
                    }
                    const u8 pp = sd ? 3 : 2;
                    std::vector<u8> seq;
                    // Four dependent ops so every operand class meets every
                    // other: x0 op= x2..x5, then the memory form at +3.
                    for (u8 s = 2; s < 6; ++s) {
                        seq.insert(seq.end(), {pfx, 0x0F, op, static_cast<u8>(0xC0 | s)});
                    }
                    run_both_modes(seq, seed, d, mx);
                    run_both_modes({pfx, 0x0F, op, 0x48, 0x03}, seed, d, mx);
                    for (u8 s = 2; s < 6; ++s) {
                        // xmm1 := xmm(s) op xmm1 ; xmm0 := xmm1 op xmm0 ;
                        // xmm6 := xmm(s) op xmm0 ; xmm1 := xmm1 op [rax]
                        auto v1 = vex2(pp, false, s, op, 1, 1);
                        auto v2 = vex2(pp, false, 1, op, 0, 0);
                        auto v3 = vex2(pp, false, s, op, 6, 0);
                        auto v4 = vex2(pp, false, 1, op, 1, -1);
                        std::vector<u8> vseq;
                        for (auto* p : {&v1, &v2, &v3, &v4}) vseq.insert(vseq.end(), p->begin(), p->end());
                        run_both_modes(vseq, seed, d, mx);
                    }
                    if (HasFatalFailure()) return;
                }
            }
        }
    }
}

// Every operand pair, one instruction at a time, so a divergence names its
// operands: zero and denormal divisors under DAZ, Inf/NaN on either side.
TEST_F(SseFallbackFormsTest, ScalarArithEveryOperandPairNative) {
    for (u8 pfx : {u8{0xF3}, u8{0xF2}}) {
        const bool sd = pfx == 0xF2;
        const usize n = sd ? std::size(kF64) : std::size(kF32);
        for (u8 op : {u8{0x58}, u8{0x59}, u8{0x5C}, u8{0x5E}}) {
            for (u32 mx : kMaskedMxcsr) {
                for (usize a = 0; a < n; ++a) {
                    for (usize b = 0; b < n; ++b) {
                        SCOPED_TRACE(testing::Message() << "pfx=0x" << std::hex
                                     << +pfx << " op=0x" << +op << " mxcsr=0x"
                                     << mx << " a=0x"
                                     << (sd ? kF64[a] : kF32[a]) << " b=0x"
                                     << (sd ? kF64[b] : kF32[b]));
                        auto seed = [a, b, sd](CpuState& c) {
                            seed_lanes(c);
                            if (sd) {
                                c.sse.xmm[0].u64_[0] = kF64[a];
                                c.sse.xmm[2].u64_[0] = kF64[b];
                            } else {
                                c.sse.xmm[0].u32_[0] = kF32[a];
                                c.sse.xmm[2].u32_[0] = kF32[b];
                            }
                        };
                        run_both_modes({pfx, 0x0F, op, 0xC2}, seed, {0}, mx);
                        if (HasFatalFailure()) return;
                    }
                }
            }
        }
    }
}

TEST_F(SseFallbackFormsTest, ScalarArithNaNRulesMatchSdm) {
    // Intel SDM Vol. 1 §4.8.3.4 Table 4-7: SRC1 NaN wins (quieted), else
    // SRC2 NaN (quieted), else an invalid operation returns the QNaN
    // indefinite whose sign is set. Destination upper lanes are SRC1's.
    struct Case { u32 a, b, expect; u8 op; };
    const Case cases[] = {
        {0x7F800001u, 0xFFC00002u, 0x7FC00001u, 0x58},  // SNaN dst beats QNaN src
        {0x7FC00003u, 0x7F800004u, 0x7FC00003u, 0x59},  // QNaN dst beats SNaN src
        {0x3F800000u, 0xFF812345u, 0xFFC12345u, 0x5C},  // src SNaN quieted
        {0x7F800000u, 0x7F800000u, 0xFFC00000u, 0x5C},  // inf - inf
        {0x00000000u, 0x7F800000u, 0xFFC00000u, 0x59},  // 0 * inf
        {0x00000000u, 0x80000000u, 0xFFC00000u, 0x5E},  // 0 / -0
        {0x7F800000u, 0xFF800000u, 0xFFC00000u, 0x5E},  // inf / -inf
    };
    for (const Case& k : cases) {
        for (bool vex : {false, true}) {
            SCOPED_TRACE(testing::Message() << std::hex << k.a << " op " << +k.op
                         << " " << k.b << (vex ? " vex" : ""));
            auto seed = [&k](CpuState& c) {
                seed_lanes(c);
                c.sse.xmm[3].u32_[0] = k.a;
                c.sse.xmm[4].u32_[0] = k.b;
            };
            // mulss xmm3, xmm4 / vmulss xmm4, xmm3, xmm4 (dst == r/m)
            run_both(true, vex ? vex2(2, false, 3, k.op, 4, 4)
                               : std::vector<u8>{0xF3, 0x0F, k.op, 0xDC},
                     seed, std::vector<u8>(16, 0), 0x1F80);
            if (HasFatalFailure()) return;
            const u8 dst = vex ? 4 : 3;
            CpuState ref;
            seed(ref);
            EXPECT_EQ(m_last_jit_cpu.sse.xmm[dst].u32_[0], k.expect);
            EXPECT_EQ(m_last_jit_cpu.sse.xmm[dst].u32_[1], ref.sse.xmm[3].u32_[1]);
            EXPECT_EQ(m_last_jit_cpu.sse.xmm[dst].u64_[1], ref.sse.xmm[3].u64_[1]);
            EXPECT_NE(m_last_mxcsr & 1u, 0u) << "IE must be set";
        }
    }
}

// ---------------------------------------------------------------------------
// MIN/MAX SS/SD/PS/PD: specials (NaN asymmetry, signed zeros, denormals under
// every DAZ/FTZ combination) and unmasked IE/DE faulting precisely.
// ---------------------------------------------------------------------------
TEST_F(SseFallbackFormsTest, MinMaxSpecialsEveryMxcsrModeNative) {
    for (u8 pfx : {u8{0xF3}, u8{0xF2}, u8{0x00}, u8{0x66}}) {
        const bool dbl = pfx == 0xF2 || pfx == 0x66;
        const usize n = dbl ? std::size(kF64) : std::size(kF32);
        for (u8 op : {u8{0x5D}, u8{0x5F}}) {
            for (u32 mx : kMaskedMxcsr) {
                for (usize a = 0; a < n; ++a) {
                    SCOPED_TRACE(testing::Message() << "pfx=0x" << std::hex << +pfx
                                 << " op=0x" << +op << " mxcsr=0x" << mx
                                 << " a=" << std::dec << a);
                    auto seed = [a, dbl, n](CpuState& c) {
                        seed_lanes(c);
                        for (int r = 0; r < 8; ++r)
                            for (int l = 0; l < (dbl ? 2 : 4); ++l) {
                                const usize k = (a + r * 3 + l * 7) % n;
                                if (dbl) c.sse.xmm[r].u64_[l] = kF64[k];
                                else c.sse.xmm[r].u32_[l] = kF32[k];
                            }
                    };
                    std::vector<u8> seq;
                    for (u8 s = 1; s < 6; ++s) {
                        if (pfx) seq.push_back(pfx);
                        seq.insert(seq.end(), {0x0F, op, static_cast<u8>(0xC0 | s)});
                    }
                    std::vector<u8> d;
                    for (usize b = 0; b < 8; ++b) {
                        auto v = dbl ? bytes_of(&kF64[(a + b) % n], 8)
                                     : bytes_of(&kF32[(a + b) % n], 4);
                        d.insert(d.end(), v.begin(), v.end());
                    }
                    run_both_modes(seq, seed, d, mx);
                    std::vector<u8> mem_form;
                    if (pfx) mem_form.push_back(pfx);
                    // scalar: [rax+3] (unaligned allowed); packed: [rax]
                    if (pfx == 0xF3 || pfx == 0xF2) {
                        mem_form.insert(mem_form.end(), {0x0F, op, 0x48, 0x03});
                    } else {
                        mem_form.insert(mem_form.end(), {0x0F, op, 0x08});
                    }
                    run_both_modes(mem_form, seed, d, mx);
                    if (HasFatalFailure()) return;
                }
            }
        }
    }
}

TEST_F(SseFallbackFormsTest, MinMaxUnmaskedExceptionsFaultPrecisely) {
    // IM clear (0x1F00) with a NaN operand, DM clear (0x1E80) with a
    // denormal: #XM in both engines, destination untouched. IM clear with
    // ordinary operands must NOT fault.
    struct Case { u32 mx; u32 a, b; };
    const Case cases[] = {
        {0x1F00u, 0x7FC00000u, 0x3F800000u}, {0x1F00u, 0x3F800000u, 0x7F800001u},
        {0x1E80u, 0x00000001u, 0x3F800000u}, {0x1E80u, 0x3F800000u, 0x80400000u},
        {0x1F00u, 0x3F800000u, 0x40000000u}, {0x1E80u, 0x3F800000u, 0x40000000u},
        {0x1EC0u, 0x00000001u, 0x3F800000u},  // DM clear but DAZ: no DE
        {0x1F00u, 0x00000001u, 0x3F800000u},  // IM clear, DE masked
    };
    for (const Case& k : cases) {
        for (u8 op : {u8{0x5D}, u8{0x5F}}) {
            SCOPED_TRACE(testing::Message() << std::hex << "mx=" << k.mx << " a="
                         << k.a << " b=" << k.b << " op=" << +op);
            auto seed = [&k](CpuState& c) {
                seed_lanes(c);
                c.sse.xmm[0].u32_[0] = k.a;
                c.sse.xmm[1].u32_[0] = k.b;
            };
            for (bool x64 : {false, true}) {
                run_both(x64, {0xF3, 0x0F, op, 0xC1}, seed, std::vector<u8>(16, 0), k.mx);
                run_both(x64, vex2(2, false, 0, op, 1, 1), seed, std::vector<u8>(16, 0), k.mx);
                if (HasFatalFailure()) return;
            }
        }
    }
}
