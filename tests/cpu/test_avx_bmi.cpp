// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// AVX / BMI / XGETBV / CPUID feature bits (Intel SDM Vol. 1–2)

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include <gtest/gtest.h>
#include <cstring>
#include <memory>
#include <vector>

using namespace vitreo;
using namespace vitreo::cpu;

class AvxBmiTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(0x1000, 0x20000, MemoryProtection::ReadWrite));
    }

    ExecResult run_code(const u8* code, usize len, CpuState& cpu) {
        EXPECT_TRUE(m_memory->write_bytes(0x1000, code, len));
        cpu.rip = 0x1000;
        Interpreter interp(cpu, *m_memory);
        return interp.step();
    }

    std::unique_ptr<GuestMemory> m_memory;
};

TEST(Decoder, VaddpsXmm) {
    std::vector<u8> data{0xC5, 0xF8, 0x58, 0xC1};
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst), DecodeError::Success);
    EXPECT_EQ(inst.opcode, Opcode::ADDPS_XMM_XMMM128);
    EXPECT_TRUE(inst.prefixes.vex);
    EXPECT_FALSE(inst.prefixes.vex_l);
}

TEST(Decoder, VaddpsYmm) {
    std::vector<u8> data{0xC5, 0xFC, 0x58, 0xC1};
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst), DecodeError::Success);
    EXPECT_EQ(inst.opcode, Opcode::ADDPS_XMM_XMMM128);
    EXPECT_TRUE(inst.prefixes.vex);
    EXPECT_TRUE(inst.prefixes.vex_l);
}

TEST_F(AvxBmiTest, Vaddps128ZerosYmmHigh) {
    CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
    for (int i = 0; i < 4; ++i) {
        cpu.sse.xmm[0].f32_[i] = 1.0f;
        cpu.sse.xmm[1].f32_[i] = 2.0f;
    }
    cpu.avx.ymm_high[0].u32_[0] = 0xDEADBEEFu;
    const u8 code[] = {0xC5, 0xF8, 0x58, 0xC1};  // vaddps xmm0, xmm0, xmm1
    ASSERT_EQ(run_code(code, sizeof(code), cpu), ExecResult::Continue);
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(cpu.sse.xmm[0].f32_[i], 3.0f);
    }
    EXPECT_EQ(cpu.avx.ymm_high[0].u32_[0], 0u);
}

TEST_F(AvxBmiTest, Vaddps256AddsHighLane) {
    CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
    for (int i = 0; i < 4; ++i) {
        cpu.sse.xmm[0].f32_[i] = 1.0f;
        cpu.sse.xmm[1].f32_[i] = 2.0f;
        cpu.avx.ymm_high[0].f32_[i] = 4.0f;
        cpu.avx.ymm_high[1].f32_[i] = 5.0f;
    }
    const u8 code[] = {0xC5, 0xFC, 0x58, 0xC1};  // vaddps ymm0, ymm0, ymm1
    ASSERT_EQ(run_code(code, sizeof(code), cpu), ExecResult::Continue);
    for (int i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(cpu.sse.xmm[0].f32_[i], 3.0f);
        EXPECT_FLOAT_EQ(cpu.avx.ymm_high[0].f32_[i], 9.0f);
    }
}

TEST_F(AvxBmiTest, AndnR32) {
    CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
    cpu.gpr.set32(0, 0xF0);
    cpu.gpr.set32(1, 0xFF);
    const u8 code[] = {0xC4, 0xE2, 0x78, 0xF2, 0xC1};  // andn eax, eax, ecx
    ASSERT_EQ(run_code(code, sizeof(code), cpu), ExecResult::Continue);
    EXPECT_EQ(cpu.gpr.get32(0), 0x0Fu);
}

TEST_F(AvxBmiTest, XgetbvReturnsXcr0) {
    CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
    cpu.gpr.set32(1, 0);  // ECX = 0
    const u8 code[] = {0x0F, 0x01, 0xD0};
    ASSERT_EQ(run_code(code, sizeof(code), cpu), ExecResult::Continue);
    EXPECT_EQ(cpu.gpr.get32(0), 7u);
    EXPECT_EQ(cpu.gpr.get32(2), 0u);
}

TEST_F(AvxBmiTest, CpuidAdvertisesAvxXsaveBmi) {
    CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
    cpu.gpr.set32(0, 1);
    cpu.gpr.set32(1, 0);
    const u8 cpuid[] = {0x0F, 0xA2};
    ASSERT_EQ(run_code(cpuid, sizeof(cpuid), cpu), ExecResult::Continue);
    EXPECT_NE(cpu.gpr.get32(1) & (1u << 26), 0u);  // XSAVE
    EXPECT_NE(cpu.gpr.get32(1) & (1u << 27), 0u);  // OSXSAVE
    EXPECT_NE(cpu.gpr.get32(1) & (1u << 28), 0u);  // AVX
    EXPECT_NE(cpu.gpr.get32(1) & (1u << 12), 0u);  // FMA

    cpu.rip = 0x1000;
    cpu.gpr.set32(0, 7);
    cpu.gpr.set32(1, 0);
    ASSERT_EQ(run_code(cpuid, sizeof(cpuid), cpu), ExecResult::Continue);
    EXPECT_NE(cpu.gpr.get32(3) & (1u << 3), 0u);   // BMI1
    EXPECT_NE(cpu.gpr.get32(3) & (1u << 5), 0u);   // AVX2
    EXPECT_NE(cpu.gpr.get32(3) & (1u << 8), 0u);   // BMI2
    EXPECT_NE(cpu.gpr.get32(3) & (1u << 19), 0u);  // ADX
}

TEST(Decoder, VpblendvbXmmIs4) {
    // c4 e3 71 4c c2 50 = vpblendvb xmm0, xmm1, xmm2, xmm5
    std::vector<u8> data{0xC4, 0xE3, 0x71, 0x4C, 0xC2, 0x50};
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst), DecodeError::Success);
    EXPECT_EQ(inst.opcode, Opcode::VPBLENDVB_XMM_XMM_XMMM128_XMM);
    EXPECT_TRUE(inst.prefixes.vex);
    EXPECT_FALSE(inst.prefixes.vex_l);
    EXPECT_EQ(inst.prefixes.vex_vvvv & 15, 1u);
    ASSERT_GE(inst.operand_count, 3u);
    EXPECT_EQ(inst.operands[0].reg & 15, 0u);
    EXPECT_EQ(inst.operands[1].reg & 15, 2u);
    EXPECT_EQ((static_cast<u8>(inst.operands[2].imm) >> 4) & 15, 5u);
}

TEST(Decoder, VblendvpsYmmIs4) {
    // c4 e3 7d 4a c1 20 = vblendvps ymm0, ymm0, ymm1, ymm2
    // (VEX.256.66.0F3A.W0 4A /r /is4 — Intel SDM Vol. 2 VBLENDVPS)
    std::vector<u8> data{0xC4, 0xE3, 0x7D, 0x4A, 0xC1, 0x20};
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst), DecodeError::Success);
    EXPECT_EQ(inst.opcode, Opcode::VBLENDVPS_YMM_YMM_YMMM256_YMM);
    EXPECT_TRUE(inst.prefixes.vex);
    EXPECT_TRUE(inst.prefixes.vex_l);
    EXPECT_EQ(inst.prefixes.vex_vvvv & 15, 0u);
    ASSERT_GE(inst.operand_count, 3u);
    EXPECT_EQ(inst.operands[0].reg & 15, 0u);
    EXPECT_EQ(inst.operands[1].reg & 15, 1u);
    EXPECT_EQ((static_cast<u8>(inst.operands[2].imm) >> 4) & 15, 2u);
}

TEST(Decoder, VblendvpdXmmIs4) {
    // c4 e3 71 4b c2 50 = vblendvpd xmm0, xmm1, xmm2, xmm5
    std::vector<u8> data{0xC4, 0xE3, 0x71, 0x4B, 0xC2, 0x50};
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst), DecodeError::Success);
    EXPECT_EQ(inst.opcode, Opcode::VBLENDVPD_XMM_XMM_XMMM128_XMM);
    EXPECT_FALSE(inst.prefixes.vex_l);
    EXPECT_EQ(inst.prefixes.vex_vvvv & 15, 1u);
    EXPECT_EQ((static_cast<u8>(inst.operands[2].imm) >> 4) & 15, 5u);
}

TEST_F(AvxBmiTest, VblendvpsYmmSelectsPerDwordBySignBit) {
    // Intel SDM Vol. 2 VBLENDVPS: each 32-bit element comes from SRC2 when the
    // corresponding mask element's most significant bit is 1, else from SRC1.
    // Only bit 31 participates; the rest of the mask element is ignored.
    const u8 code[] = {0xC4, 0xE3, 0x7D, 0x4A, 0xC1, 0x20};  // ymm0,ymm0,ymm1,ymm2
    CpuState cpu;
    for (int i = 0; i < 4; ++i) {
        cpu.sse.xmm[0].u32_[i] = 0x11111111u;   // SRC1 low
        cpu.avx.ymm_high[0].u32_[i] = 0x22222222u;
        cpu.sse.xmm[1].u32_[i] = 0xAAAAAAAAu;   // SRC2 low
        cpu.avx.ymm_high[1].u32_[i] = 0xBBBBBBBBu;
        // Alternate the sign bit; leave the low bits set so a byte- or
        // qword-wide blend would produce a visibly different answer.
        cpu.sse.xmm[2].u32_[i] = (i % 2) ? 0x80000001u : 0x7FFFFFFFu;
        cpu.avx.ymm_high[2].u32_[i] = (i % 2) ? 0x7FFFFFFFu : 0x80000001u;
    }
    ASSERT_EQ(run_code(code, sizeof(code), cpu), ExecResult::Continue);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(cpu.sse.xmm[0].u32_[i],
                  (i % 2) ? 0xAAAAAAAAu : 0x11111111u);
        EXPECT_EQ(cpu.avx.ymm_high[0].u32_[i],
                  (i % 2) ? 0x22222222u : 0xBBBBBBBBu);
    }
}

TEST_F(AvxBmiTest, VblendvpdXmmSelectsPerQwordAndZeroesYmmHigh) {
    // c4 e3 71 4b c2 50 = vblendvpd xmm0, xmm1, xmm2, xmm5
    const u8 code[] = {0xC4, 0xE3, 0x71, 0x4B, 0xC2, 0x50};
    CpuState cpu;
    cpu.sse.xmm[1].u64_[0] = 0x1111111111111111ull;  // SRC1
    cpu.sse.xmm[1].u64_[1] = 0x2222222222222222ull;
    cpu.sse.xmm[2].u64_[0] = 0xAAAAAAAAAAAAAAAAull;  // SRC2
    cpu.sse.xmm[2].u64_[1] = 0xBBBBBBBBBBBBBBBBull;
    cpu.sse.xmm[5].u64_[0] = 0x8000000000000000ull;  // mask: select SRC2
    cpu.sse.xmm[5].u64_[1] = 0x7FFFFFFFFFFFFFFFull;  // mask: select SRC1
    cpu.avx.ymm_high[0].u64_[0] = 0xDEADBEEFDEADBEEFull;
    ASSERT_EQ(run_code(code, sizeof(code), cpu), ExecResult::Continue);
    EXPECT_EQ(cpu.sse.xmm[0].u64_[0], 0xAAAAAAAAAAAAAAAAull);
    EXPECT_EQ(cpu.sse.xmm[0].u64_[1], 0x2222222222222222ull);
    // VEX.128 clears the upper 128 bits of the destination.
    EXPECT_EQ(cpu.avx.ymm_high[0].u64_[0], 0ull);
}

TEST(Decoder, VpmovmskbYmm) {
    // c5 fd d7 c0 = vpmovmskb eax, ymm0 (VEX.256.66.0F D7)
    std::vector<u8> data{0xC5, 0xFD, 0xD7, 0xC0};
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst), DecodeError::Success);
    EXPECT_EQ(inst.opcode, Opcode::PMOVMSKB_R32_XMM);
    EXPECT_TRUE(inst.prefixes.vex);
    EXPECT_TRUE(inst.prefixes.vex_l);
}

TEST(Decoder, PmovmskbOperandFormsAndReservedFieldsBothModes) {
    // SDM 2B 4-348/349, confirmed by pmovmskb_encoding_probe.expected.
    const std::vector<std::vector<u8>> prefixes{
        {}, {0x66}, {0xF2}, {0xF3}, {0xF2,0x66}, {0xF3,0x66},
        {0x66,0xF2}, {0x66,0xF3}};
    for (const auto mode : {DecodeMode::Mode32, DecodeMode::Mode64}) {
        for (usize p = 0; p < prefixes.size(); ++p) for (bool memory : {false,true}) {
            auto code = prefixes[p];
            code.insert(code.end(), {0x0F,0xD7,static_cast<u8>(memory ? 0x01 : 0xC1)});
            MemoryByteReader reader(code.data(), code.size()); DecodedInstruction inst;
            EXPECT_EQ(decode_instruction(reader, mode, inst),
                      !memory && p < 2 ? DecodeError::Success : DecodeError::InvalidOpcode);
        }
        for (u8 pp = 0; pp < 4; ++pp) for (bool l : {false,true})
        for (bool w : {false,true}) for (u8 v = 0; v < 16; ++v)
        for (bool memory : {false,true}) for (bool short_vex : {false,true}) {
            // SDM 2.3.6: in 32-bit mode C5 is VEX only when the next
            // byte's top two bits are 11; other patterns decode as LDS.
            if (short_vex && (w || (mode == DecodeMode::Mode32 && v >= 8))) continue;
            const u8 control = static_cast<u8>(((~v & 15) << 3) | (l ? 4 : 0) | pp);
            std::vector<u8> code = short_vex ? std::vector<u8>{0xC5,static_cast<u8>(control | 0x80)}
                                          : std::vector<u8>{0xC4,0xE1,static_cast<u8>(control | (w ? 0x80 : 0))};
            code.insert(code.end(), {0xD7,static_cast<u8>(memory ? 0x01 : 0xC1)});
            MemoryByteReader reader(code.data(), code.size()); DecodedInstruction inst;
            EXPECT_EQ(decode_instruction(reader, mode, inst),
                      pp == 1 && v == 0 && !memory ? DecodeError::Success : DecodeError::InvalidOpcode)
                << "pp=" << unsigned(pp) << " L=" << l << " W=" << w
                << " vvvv=" << unsigned(v) << " memory=" << memory;
        }
    }
}

TEST(PmovmskbContract, EveryByteMaskPreservesSourcesAndFlagsBothModesAndTiers) {
    // SDM equations: result bit i is source byte i bit 7; every higher
    // destination bit is zero. No vector register or flag is written.
    for (bool x64 : {false,true}) for (unsigned form : {0u,1u,2u})
    for (bool jit_tier : {false,true}) for (bool w : {false,true}) {
        if (form == 0 && !x64 && w) continue;
        const u8 src = x64 ? 9 : 1, dst = x64 ? 8 : 0;
        std::vector<u8> code;
        if (form == 0) {
            code = {0x66};
            if (x64) code.push_back(w ? 0x4D : 0x45);
            code.insert(code.end(), {0x0F,0xD7,0xC1});
        } else {
            code = {0xC4,static_cast<u8>(x64 ? 0x41 : 0xE1),
                    static_cast<u8>(0x79 | (w ? 0x80 : 0) | (form == 2 ? 4 : 0)),0xD7,0xC1};
        }
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000,0x4000,MemoryProtection::ReadWriteExecute));
        ASSERT_TRUE(memory.write_bytes(0x1000,code.data(),code.size()));
        MemoryByteReader reader(code.data(),code.size()); DecodedInstruction inst;
        ASSERT_EQ(decode_instruction(reader,x64 ? DecodeMode::Mode64 : DecodeMode::Mode32,inst),DecodeError::Success);
        EXPECT_EQ(inst.operands[0].reg,dst); EXPECT_EQ(inst.operands[1].reg,src);
        cpu::jit::JitCompiler jit(memory); cpu::jit::CompiledBlock* compiled = nullptr;
        if (jit_tier) {
            cpu::jit::BasicBlock block; block.start_addr=0x1000; block.is_64bit=x64;
            block.byte_length=static_cast<u32>(code.size()); block.instruction_count=1; block.instructions.push_back(inst);
            compiled=jit.compile(block); ASSERT_NE(compiled,nullptr);
        }
        const unsigned bytes = form == 2 ? 32 : 16;
        for (unsigned sign = 0; sign <= bytes + 1; ++sign) {
            CpuState cpu;
            if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
            cpu.rip=0x1000; cpu.gpr.set64(4,0x4000); cpu.gpr.set64(dst,~u64{0});
            cpu.flags.rflags=0x202 | FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF;
            u32 expected=0;
            for (unsigned b=0;b<32;++b) {
                const bool high=b<bytes && (sign==b || sign==bytes+1);
                auto& lane=b<16 ? cpu.sse.xmm[src] : cpu.avx.ymm_high[src];
                lane.u8_[b%16]=static_cast<u8>(0x55 | (high ? 0x80 : 0));
                if (high) expected |= u32{1} << b;
            }
            const CpuState before=cpu;
            if (jit_tier) {
                EXPECT_EQ(jit.execute(*compiled,cpu),cpu::jit::JitExecResult::Continue);
                EXPECT_FALSE(cpu.pending_raise.active);
            } else { Interpreter interpreter(cpu,memory); EXPECT_EQ(interpreter.step(),ExecResult::Continue); }
            cpu.materialize_lazy_flags();
            EXPECT_EQ(x64 ? cpu.gpr.get64(dst) : cpu.gpr.get32(dst),expected);
            EXPECT_EQ(cpu.flags.rflags,before.flags.rflags);
            EXPECT_EQ(std::memcmp(cpu.sse.xmm,before.sse.xmm,sizeof(cpu.sse.xmm)),0);
            EXPECT_EQ(std::memcmp(cpu.avx.ymm_high,before.avx.ymm_high,sizeof(cpu.avx.ymm_high)),0);
            for (u8 r=0;r<(x64 ? 16 : 8);++r) if (r!=dst) EXPECT_EQ(cpu.gpr.get64(r),before.gpr.get64(r));
        }
    }
}

TEST_F(AvxBmiTest, VpmovmskbYmmConcatenatesHighLaneMask) {
    // Intel SDM Vol. 2 VPMOVMSKB ymm: r32[15:0] from SRC[127:0], r32[31:16]
    // from SRC[255:128]. The AVX SSE bridge used to overwrite the low mask
    // with the high-lane extract, which made AVX2 wcslen report a NUL 8
    // wchars early when only the high 16 bytes were zeros.
    CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
    cpu.gpr.set64(0, 0xFFFFFFFFFFFFFFFFull);
    for (int i = 0; i < 16; ++i) {
        cpu.sse.xmm[0].u8_[i] = 0x01;
        cpu.avx.ymm_high[0].u8_[i] = 0x01;
    }
    cpu.avx.ymm_high[0].u8_[0] = 0x80;
    const u8 code[] = {0xC5, 0xFD, 0xD7, 0xC0};  // vpmovmskb eax, ymm0
    ASSERT_EQ(run_code(code, sizeof(code), cpu), ExecResult::Continue);
    EXPECT_EQ(cpu.gpr.get64(0), 0x00010000ull);

    cpu.rip = 0x1000;
    cpu.gpr.set64(0, 0xFFFFFFFFFFFFFFFFull);
    cpu.avx.ymm_high[0].u8_[0] = 0x01;
    cpu.sse.xmm[0].u8_[0] = 0x80;
    ASSERT_EQ(run_code(code, sizeof(code), cpu), ExecResult::Continue);
    EXPECT_EQ(cpu.gpr.get64(0), 0x00000001ull);
}

TEST_F(AvxBmiTest, VpcmpeqwYmmPmovmskbFindsNulInHighLane) {
    // AVX2 wcslen: vpxor ymm2,ymm2,ymm2; vpcmpeqw ymm1,ymm2,[rdx];
    // vpmovmskb eax,ymm1. A NUL only in bytes [16:17] must set bits 16-17.
    CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
    cpu.gpr.set64(static_cast<u8>(RegIndex::RDX), 0x3000);
    u8 buf[32];
    std::memset(buf, 0x61, sizeof(buf));
    buf[16] = 0;
    buf[17] = 0;
    ASSERT_TRUE(m_memory->write_bytes(0x3000, buf, sizeof(buf)).is_ok());
    const u8 code[] = {
        0xC5, 0xED, 0x57, 0xD2,  // vpxor ymm2, ymm2, ymm2
        0xC5, 0xED, 0x75, 0x0A,  // vpcmpeqw ymm1, ymm2, [rdx]
        0xC5, 0xFD, 0xD7, 0xC1,  // vpmovmskb eax, ymm1
    };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)).is_ok());
    cpu.rip = 0x1000;
    Interpreter interp(cpu, *m_memory);
    ASSERT_EQ(interp.run(3), ExecResult::Continue);
    EXPECT_EQ(cpu.gpr.get32(0), 0x00030000u);
}

TEST_F(AvxBmiTest, Vpbroadcastb256MemchrKernelReportsOnlyRealMatches) {
    // AVX2 memchr: vpbroadcastb ymm0, xmm0; vmovdqu ymm1,[rdi];
    // vpcmpeqb ymm1, ymm1, ymm0; vpmovmskb eax, ymm1. A buffer with no
    // needle must yield a zero mask; a stale high lane anywhere in that
    // chain invents matches past byte 15 (protobuf then rejects every
    // symbol name as "contains null character").
    auto run_kernel = [&](const u8* buf, u8 needle) -> u32 {
        CpuState cpu;
        cpu.init_long_mode();
        cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
        cpu.gpr.set64(static_cast<u8>(RegIndex::RDI), 0x3000);
        // Poison every high lane so a missing 256-bit path is visible.
        for (int r = 0; r < 16; ++r) {
            for (int i = 0; i < 16; ++i) {
                cpu.avx.ymm_high[r].u8_[i] = 0x5A;
                cpu.sse.xmm[r].u8_[i] = 0x5A;
            }
        }
        cpu.sse.xmm[0].u8_[0] = needle;
        EXPECT_TRUE(m_memory->write_bytes(0x3000, buf, 32).is_ok());
        const u8 code[] = {
            0xC4, 0xE2, 0x7D, 0x78, 0xC0,  // vpbroadcastb ymm0, xmm0
            0xC5, 0xFE, 0x6F, 0x0F,        // vmovdqu ymm1, [rdi]
            0xC5, 0xF5, 0x74, 0xC8,        // vpcmpeqb ymm1, ymm1, ymm0
            0xC5, 0xFD, 0xD7, 0xC1,        // vpmovmskb eax, ymm1
        };
        EXPECT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)).is_ok());
        cpu.rip = 0x1000;
        Interpreter interp(cpu, *m_memory);
        EXPECT_EQ(interp.run(4), ExecResult::Continue);
        return cpu.gpr.get32(0);
    };

    u8 buf[32];
    std::memset(buf, 0x61, sizeof(buf));
    EXPECT_EQ(run_kernel(buf, 0x00), 0u) << "no NUL in the buffer";

    buf[20] = 0x00;
    EXPECT_EQ(run_kernel(buf, 0x00), 1u << 20) << "NUL only at byte 20";

    std::memset(buf, 0x61, sizeof(buf));
    buf[3] = 0x00;
    EXPECT_EQ(run_kernel(buf, 0x00), 1u << 3) << "NUL only at byte 3";
}

TEST_F(AvxBmiTest, Vpblendvb128SelectsByMaskMsb) {
    CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
    for (int i = 0; i < 16; ++i) {
        cpu.sse.xmm[1].u8_[i] = static_cast<u8>(0x10 + i);  // src1
        cpu.sse.xmm[2].u8_[i] = static_cast<u8>(0xA0 + i);  // src2
        cpu.sse.xmm[5].u8_[i] = (i & 1) ? 0x80 : 0x00;       // mask
    }
    cpu.avx.ymm_high[0].u32_[0] = 0xDEADBEEFu;
    // vpblendvb xmm0, xmm1, xmm2, xmm5
    const u8 code[] = {0xC4, 0xE3, 0x71, 0x4C, 0xC2, 0x50};
    ASSERT_EQ(run_code(code, sizeof(code), cpu), ExecResult::Continue);
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(cpu.sse.xmm[0].u8_[i],
                  (i & 1) ? static_cast<u8>(0xA0 + i) : static_cast<u8>(0x10 + i));
    }
    EXPECT_EQ(cpu.avx.ymm_high[0].u32_[0], 0u);
}

TEST(Decoder, VpmaskmovdYmmLoad) {
    // c4 e2 6d 8c 01 = vpmaskmovd ymm0, ymm2, [rcx]
    std::vector<u8> data{0xC4, 0xE2, 0x6D, 0x8C, 0x01};
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst), DecodeError::Success);
    EXPECT_EQ(inst.opcode, Opcode::VPMASKMOVD_LOAD);
    EXPECT_TRUE(inst.prefixes.vex);
    EXPECT_TRUE(inst.prefixes.vex_l);
    EXPECT_EQ(inst.prefixes.vex_vvvv & 15, 2u);
    ASSERT_GE(inst.operand_count, 2u);
    EXPECT_EQ(inst.operands[0].reg & 15, 0u);
    EXPECT_TRUE(is_memory_operand(inst.operands[1].type));
}

TEST_F(AvxBmiTest, VpmaskmovdYmmLoadZerosUnmaskedLanes) {
    // Intel SDM Vol. 2 VPMASKMOVD 256-bit load: masked dwords come from
    // memory; unmasked dest lanes are zeroed (not merged).
    CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
    cpu.gpr.set64(static_cast<u8>(RegIndex::RCX), 0x3000);
    u32 src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_TRUE(m_memory->write_bytes(
        0x3000, reinterpret_cast<const u8*>(src), sizeof(src)).is_ok());
    for (int i = 0; i < 4; ++i) {
        cpu.sse.xmm[0].u32_[i] = 0xFFFFFFFFu;
        cpu.avx.ymm_high[0].u32_[i] = 0xFFFFFFFFu;
        cpu.sse.xmm[2].u32_[i] = (i & 1) ? 0x80000000u : 0;
        cpu.avx.ymm_high[2].u32_[i] = (i == 0) ? 0x80000000u : 0;
    }
    const u8 code[] = {0xC4, 0xE2, 0x6D, 0x8C, 0x01};
    ASSERT_EQ(run_code(code, sizeof(code), cpu), ExecResult::Continue);
    EXPECT_EQ(cpu.sse.xmm[0].u32_[0], 0u);
    EXPECT_EQ(cpu.sse.xmm[0].u32_[1], 2u);
    EXPECT_EQ(cpu.sse.xmm[0].u32_[2], 0u);
    EXPECT_EQ(cpu.sse.xmm[0].u32_[3], 4u);
    EXPECT_EQ(cpu.avx.ymm_high[0].u32_[0], 5u);
    EXPECT_EQ(cpu.avx.ymm_high[0].u32_[1], 0u);
    EXPECT_EQ(cpu.avx.ymm_high[0].u32_[2], 0u);
    EXPECT_EQ(cpu.avx.ymm_high[0].u32_[3], 0u);
}

TEST_F(AvxBmiTest, VpmaskmovdYmmLoadSkipsUnmaskedFault) {
    // Unmapped bytes past the first 16 must not fault when those dwords
    // are unmasked.
    ASSERT_TRUE(m_memory->map_region(0x40000, 16, MemoryProtection::ReadWrite));
    CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
    cpu.gpr.set64(static_cast<u8>(RegIndex::RCX), 0x40000);
    u32 src[4] = {0x11, 0x22, 0x33, 0x44};
    ASSERT_TRUE(m_memory->write_bytes(
        0x40000, reinterpret_cast<const u8*>(src), sizeof(src)).is_ok());
    for (int i = 0; i < 4; ++i) {
        cpu.sse.xmm[2].u32_[i] = 0x80000000u;
        cpu.avx.ymm_high[2].u32_[i] = 0;
        cpu.sse.xmm[0].u32_[i] = 0xDEADBEEFu;
        cpu.avx.ymm_high[0].u32_[i] = 0xCAFEBABEu;
    }
    const u8 code[] = {0xC4, 0xE2, 0x6D, 0x8C, 0x01};
    ASSERT_EQ(run_code(code, sizeof(code), cpu), ExecResult::Continue);
    EXPECT_EQ(cpu.sse.xmm[0].u32_[0], 0x11u);
    EXPECT_EQ(cpu.sse.xmm[0].u32_[3], 0x44u);
    EXPECT_EQ(cpu.avx.ymm_high[0].u32_[0], 0u);
    EXPECT_EQ(cpu.avx.ymm_high[0].u32_[3], 0u);
}

TEST_F(AvxBmiTest, VpmaskmovdYmmStoreWritesMaskedLanesOnly) {
    CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
    cpu.gpr.set64(static_cast<u8>(RegIndex::RCX), 0x3000);
    u32 fill[8];
    for (int i = 0; i < 8; ++i) fill[i] = 0xA0A0A0A0u;
    ASSERT_TRUE(m_memory->write_bytes(
        0x3000, reinterpret_cast<const u8*>(fill), sizeof(fill)).is_ok());
    for (int i = 0; i < 4; ++i) {
        cpu.sse.xmm[0].u32_[i] = 0x100u + static_cast<u32>(i);
        cpu.avx.ymm_high[0].u32_[i] = 0x200u + static_cast<u32>(i);
        cpu.sse.xmm[2].u32_[i] = (i == 1) ? 0x80000000u : 0;
        cpu.avx.ymm_high[2].u32_[i] = (i == 2) ? 0x80000000u : 0;
    }
    const u8 code[] = {0xC4, 0xE2, 0x6D, 0x8E, 0x01};
    ASSERT_EQ(run_code(code, sizeof(code), cpu), ExecResult::Continue);
    u32 after[8] = {};
    ASSERT_TRUE(m_memory->read_bytes(
        0x3000, reinterpret_cast<u8*>(after), sizeof(after)).is_ok());
    EXPECT_EQ(after[0], 0xA0A0A0A0u);
    EXPECT_EQ(after[1], 0x101u);
    EXPECT_EQ(after[2], 0xA0A0A0A0u);
    EXPECT_EQ(after[6], 0x202u);
    EXPECT_EQ(after[7], 0xA0A0A0A0u);
}
