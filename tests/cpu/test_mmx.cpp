// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// MMX instruction regression tests (CPU-045)

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include <functional>
#include <gtest/gtest.h>

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

static DecodedInstruction decode(const std::initializer_list<u8>& bytes,
                                 DecodeMode mode = DecodeMode::Mode32) {
    std::vector<u8> data(bytes);
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    DecodeError err = decode_instruction(reader, mode, inst);
    EXPECT_EQ(err, DecodeError::Success) << "Decode failed";
    return inst;
}

class MmxTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        auto map_result = m_memory->map_region(0x1000, 0x20000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(map_result);

        m_cpu = std::make_unique<CpuState>();
        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(static_cast<u8>(RegIndex::ESP), 0x11000);
        m_cpu->rip = 0x1000;
    }

    void write_code(const std::initializer_list<u8>& bytes) {
        u64 addr = 0x1000;
        for (u8 byte : bytes) {
            ASSERT_TRUE(m_memory->write_u8(addr++, byte));
        }
    }

    ExecResult step() {
        Interpreter interp(*m_cpu, *m_memory);
        return interp.step();
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<CpuState> m_cpu;
};

TEST(MmxDecoder, PunpckldqMmMm) {
    auto inst = decode({0x0F, 0x62, 0xF9});
    EXPECT_EQ(inst.opcode, Opcode::PUNPCKLDQ_MM_MMM64);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[0].reg, 7);
    EXPECT_EQ(inst.operands[1].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[1].reg, 1);
}

TEST(MmxDecoder, PunpckhqdqMmMm) {
    // NP 0F 6D — MM form (XMM uses 66 prefix). Sole qword → dst ← src.
    auto inst = decode({0x0F, 0x6D, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::PUNPCKHQDQ_MM_MMM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[1].reg, 1);
}

TEST_F(MmxTest, PunpckldqInterleavesLowDwords) {
    m_cpu->fpu.set_mm(7, 0x1122334455667788ull);
    m_cpu->fpu.set_mm(1, 0x99aabbccddeeff00ull);
    write_code({0x0F, 0x62, 0xF9});

    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(7), 0xddeeff0055667788ull);
    EXPECT_EQ(m_cpu->rip, 0x1003u);
}

TEST_F(MmxTest, PunpckhqdqCopiesSrcQword) {
    // MMX has only one qword; high unpack → dst ← src.
    m_cpu->fpu.set_mm(0, 0x1122334455667788ull);
    m_cpu->fpu.set_mm(1, 0x99aabbccddeeff00ull);
    write_code({0x0F, 0x6D, 0xC1});

    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0), 0x99aabbccddeeff00ull);
    EXPECT_EQ(m_cpu->fpu.get_mm(1), 0x99aabbccddeeff00ull);
}

// ---------------------------------------------------------------------------
// MMX byte/word unpacks (0F 60/61/68/69 with no 66 prefix).  These were
// previously mis-decoded as InvalidOpcode, so smackw32's MMX pixel-doubling
// blit (punpcklbw mm0, mm0) silently did nothing → garbled Smacker video.
// ---------------------------------------------------------------------------
TEST(MmxDecoder, PunpcklbwMmMm) {
    auto inst = decode({0x0F, 0x60, 0xF9});
    EXPECT_EQ(inst.opcode, Opcode::PUNPCKLBW_MM_MMM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[0].reg, 7);
    EXPECT_EQ(inst.operands[1].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[1].reg, 1);
}

TEST_F(MmxTest, PunpcklbwInterleavesLowBytes) {
    m_cpu->fpu.set_mm(7, 0x1122334455667788ull);
    m_cpu->fpu.set_mm(1, 0x99aabbccddeeff00ull);
    write_code({0x0F, 0x60, 0xF9});  // punpcklbw mm7, mm1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(7), 0xdd55ee66ff770088ull);
}

TEST_F(MmxTest, PunpckhbwInterleavesHighBytes) {
    m_cpu->fpu.set_mm(7, 0x1122334455667788ull);
    m_cpu->fpu.set_mm(1, 0x99aabbccddeeff00ull);
    write_code({0x0F, 0x68, 0xF9});  // punpckhbw mm7, mm1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(7), 0x9911aa22bb33cc44ull);
}

TEST_F(MmxTest, PunpcklwdInterleavesLowWords) {
    m_cpu->fpu.set_mm(7, 0x1122334455667788ull);
    m_cpu->fpu.set_mm(1, 0x99aabbccddeeff00ull);
    write_code({0x0F, 0x61, 0xF9});  // punpcklwd mm7, mm1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(7), 0xddee5566ff007788ull);
}

TEST_F(MmxTest, PunpckhwdInterleavesHighWords) {
    m_cpu->fpu.set_mm(7, 0x1122334455667788ull);
    m_cpu->fpu.set_mm(1, 0x99aabbccddeeff00ull);
    write_code({0x0F, 0x69, 0xF9});  // punpckhwd mm7, mm1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(7), 0x99aa1122bbcc3344ull);
}

// The exact Smacker 2x pixel-doubling idiom: punpcklbw mm0, mm0 doubles each
// of the low 4 bytes.  [.. 55 66 77 88] -> 55 55 66 66 77 77 88 88.
TEST_F(MmxTest, PunpcklbwSelfDoublesLowBytes_SmackerIdiom) {
    m_cpu->fpu.set_mm(0, 0x1122334455667788ull);
    write_code({0x0F, 0x60, 0xC0});  // punpcklbw mm0, mm0
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0), 0x5555666677778888ull);
}

// ---------------------------------------------------------------------------
// MMX PANDN / PCMPEQ (0F DF/74/75/76 with no 66 prefix). Previously the MMX
// forms were undecodable (PANDN mis-mapped to XMM; PCMPEQ -> InvalidOpcode),
// breaking smackw32's color-keyed transparent blit that composites menu movies.
// ---------------------------------------------------------------------------
TEST(MmxDecoder, PandnMmMm) {
    auto inst = decode({0x0F, 0xDF, 0xF9});
    EXPECT_EQ(inst.opcode, Opcode::PANDN_MM_MMM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[0].reg, 7);
    EXPECT_EQ(inst.operands[1].reg, 1);
}

TEST_F(MmxTest, PandnComputesNotDstAndSrc) {
    m_cpu->fpu.set_mm(7, 0x00000000FFFFFFFFull);
    m_cpu->fpu.set_mm(1, 0xAAAAAAAAAAAAAAAAull);
    write_code({0x0F, 0xDF, 0xF9});  // pandn mm7, mm1  => (~mm7) & mm1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(7), 0xAAAAAAAA00000000ull);
}

TEST_F(MmxTest, PcmpeqbSetsAllOnesForEqualBytes) {
    m_cpu->fpu.set_mm(7, 0x1122334455667788ull);
    m_cpu->fpu.set_mm(1, 0x1100330055007700ull);  // bytes 0,2,4,6 differ; 1,3,5,7 equal
    write_code({0x0F, 0x74, 0xF9});  // pcmpeqb mm7, mm1
    EXPECT_EQ(step(), ExecResult::Continue);
    // equal bytes -> 0xFF, unequal -> 0x00 (byte i equal when both have 0x?? match)
    EXPECT_EQ(m_cpu->fpu.get_mm(7), 0xFF00FF00FF00FF00ull);
}

TEST_F(MmxTest, PcmpeqdSetsAllOnesForEqualDwords) {
    m_cpu->fpu.set_mm(7, 0x11223344AABBCCDDull);
    m_cpu->fpu.set_mm(1, 0x11223344FFFFFFFFull);  // high dword equal, low differs
    write_code({0x0F, 0x76, 0xF9});  // pcmpeqd mm7, mm1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(7), 0xFFFFFFFF00000000ull);
}

// NP 0F 64/65/66 are MMX PCMPGTB/W/D. The 66-prefixed forms are SSE2 XMM;
// treating the no-prefix encoding as #UD was the lot-load crash.
TEST(MmxDecoder, PcmpgtwMmMm) {
    // 0F 65 D0 — PCMPGTW mm2, mm0
    auto inst = decode({0x0F, 0x65, 0xD0});
    EXPECT_EQ(inst.opcode, Opcode::PCMPGTW_MM_MMM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[0].reg, 2);
    EXPECT_EQ(inst.operands[1].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[1].reg, 0);
}

TEST_F(MmxTest, PcmpgtwSignedWords) {
    // words (lo→hi): 2>-1? no wait:
    // mm2:  2, -2, 0, 32767
    // mm0:  1, -1, 0, -32768
    //  2>1, -2>-1? F, 0>0? F, 32767>-32768
    m_cpu->fpu.set_mm(2, 0x7FFF0000FFFE0002ull);
    m_cpu->fpu.set_mm(0, 0x80000000FFFF0001ull);
    write_code({0x0F, 0x65, 0xD0});  // pcmpgtw mm2, mm0
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(2), 0xFFFF00000000FFFFull);
}

TEST_F(MmxTest, PcmpgtbSignedBytes) {
    m_cpu->fpu.set_mm(0, 0x7F00FF05ull);       // … 127, 0, -1, 5
    m_cpu->fpu.set_mm(1, 0x80000103ull);       // … -128, 0, 1, 3
    write_code({0x0F, 0x64, 0xC1});  // pcmpgtb mm0, mm1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0) & 0xFFFFFFFFull, 0xFF0000FFull);
}

TEST_F(MmxTest, PcmpgtdSignedDwords) {
    m_cpu->fpu.set_mm(0, 0xFFFFFFCE00000064ull);  // 100, -50
    m_cpu->fpu.set_mm(1, 0xFFFFFFF600000032ull);  // 50, -10
    write_code({0x0F, 0x66, 0xC1});  // pcmpgtd mm0, mm1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0), 0x00000000FFFFFFFFull);
}

// NP 0F F9/EA/EE/70/6B are MMX (or SSE-on-MMX) and must write MM0-7, not the
// XMM8-15 alias. The lot-load DIV-by-zero came from PSUBW leaving MM3 at 0
// so `pcmpeqw; psubw; movd esi, mm3` never lifted a zero low word to 1.
TEST(MmxDecoder, PsubwMmMm) {
    auto inst = decode({0x0F, 0xF9, 0xC1});  // PSUBW mm0, mm1
    EXPECT_EQ(inst.opcode, Opcode::PSUBW_MM_MMM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[1].reg, 1);
}

TEST(MmxDecoder, PmaxswPminswPshufwPackssdwMm) {
    auto pmax = decode({0x0F, 0xEE, 0xC2});  // PMAXSW mm0, mm2
    EXPECT_EQ(pmax.opcode, Opcode::PMAXSW_MM_MMM64);
    EXPECT_EQ(pmax.operands[0].type, OperandType::RegMM);

    auto pmin = decode({0x0F, 0xEA, 0xCB});  // PMINSW mm1, mm3
    EXPECT_EQ(pmin.opcode, Opcode::PMINSW_MM_MMM64);
    EXPECT_EQ(pmin.operands[0].type, OperandType::RegMM);
    EXPECT_EQ(pmin.operands[1].reg, 3);

    auto pshuf = decode({0x0F, 0x70, 0xC0, 0xB1});  // PSHUFW mm0, mm0, 0xB1
    EXPECT_EQ(pshuf.opcode, Opcode::PSHUFW_MM_MMM64_IMM8);
    EXPECT_EQ(pshuf.operands[0].type, OperandType::RegMM);
    EXPECT_EQ(pshuf.operands[0].reg, 0);
    EXPECT_EQ(pshuf.operands[2].imm, 0xB1u);

    auto pack = decode({0x0F, 0x6B, 0xC1});  // PACKSSDW mm0, mm1
    EXPECT_EQ(pack.opcode, Opcode::PACKSSDW_MM_MMM64);
    EXPECT_EQ(pack.operands[0].type, OperandType::RegMM);
}

TEST_F(MmxTest, PsubwWrapsAndFeedsDivGuard) {
    // 0 - 0xFFFF wraps to 1, which is how guests avoid a zero DIV source.
    m_cpu->fpu.set_mm(3, 0);
    m_cpu->fpu.set_mm(1, 0);
    m_cpu->fpu.set_mm(7, 0);
    m_cpu->sse.xmm[8 + 3].u64_[0] = 0xDEADBEEFDEADBEEFull;
    m_cpu->sse.xmm[8 + 3].u64_[1] = 0xDEADBEEFDEADBEEFull;

    write_code({
        0x0F, 0xEF, 0xFF,        // pxor mm7, mm7
        0x0F, 0xF9, 0xD9,        // psubw mm3, mm1   ; 0-0 = 0
        0x0F, 0x75, 0xFB,        // pcmpeqw mm7, mm3 ; FFFF in every word
        0x0F, 0xF9, 0xDF,        // psubw mm3, mm7   ; 0 - FFFF = 1
        0x0F, 0x7E, 0xDE,        // movd esi, mm3
    });

    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(step(), ExecResult::Continue) << "Failed at step " << i;
    }
    EXPECT_EQ(m_cpu->fpu.get_mm(3), 0x0001000100010001ull);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::ESI)), 0x00010001u);
    EXPECT_EQ(m_cpu->sse.xmm[8 + 3].u64_[0], 0xDEADBEEFDEADBEEFull);
}

TEST_F(MmxTest, PmaxswPminswSignedWords) {
    m_cpu->fpu.set_mm(0, 0x0002FFFF00010000ull);  // 0, 1, -1, 2
    m_cpu->fpu.set_mm(2, 0x0003FFFE00000005ull);  // 5, 0, -2, 3
    write_code({0x0F, 0xEE, 0xC2});  // pmaxsw mm0, mm2
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0), 0x0003FFFF00010005ull);  // 5, 1, -1, 3

    m_cpu->rip = 0x1000;
    m_cpu->fpu.set_mm(1, 0x0002FFFF00010000ull);
    m_cpu->fpu.set_mm(3, 0x0003FFFE00000005ull);
    write_code({0x0F, 0xEA, 0xCB});  // pminsw mm1, mm3
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(1), 0x0002FFFE00000000ull);  // 0, 0, -2, 2
}

TEST_F(MmxTest, PshufwSwapsAdjacentWords) {
    // imm 0xB1 = 10 11 00 01 → [w1, w0, w3, w2]
    m_cpu->fpu.set_mm(0, 0x0004000300020001ull);
    m_cpu->sse.xmm[0].u64_[0] = 0xAAAAAAAAAAAAAAAAULL;
    write_code({0x0F, 0x70, 0xC0, 0xB1});  // pshufw mm0, mm0, 0xB1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0), 0x0003000400010002ull);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0xAAAAAAAAAAAAAAAAULL);
}

TEST_F(MmxTest, PackssdwSaturatesToInt16) {
    m_cpu->fpu.set_mm(0, 0x000080000000007Full);  // 127, 32768 → 127, 32767
    m_cpu->fpu.set_mm(1, 0xFFFF8000FFFFFFFFull);  // -1, -32768
    write_code({0x0F, 0x6B, 0xC1});  // packssdw mm0, mm1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0), 0x8000FFFF7FFF007Full);
}

TEST_F(MmxTest, SteamSequencePrefixExecutesWithoutException) {
    m_cpu->fpu.set_mm(7, 0x934a94cb32fb18e8ull);
    m_cpu->fpu.set_mm(1, 0x0102030405060708ull);
    m_cpu->fpu.set_mm(4, 0xaabbccddeeff0011ull);
    m_cpu->fpu.set_mm(5, 0xf0f0f0f00f0f0f0full);
    m_cpu->fpu.set_mm(6, 0x00ff00ff00ff00ffull);

    write_code({
        0x0F, 0x62, 0xF9,             // punpckldq mm7, mm1
        0x0F, 0x6F, 0xCC,             // movq mm1, mm4
        0x0F, 0xEF, 0xEE,             // pxor mm5, mm6
        0x0F, 0x73, 0xD1, 0x0E,       // psrlq mm1, 14
        0x0F, 0x7F, 0x64, 0x24, 0x20  // movq [esp+0x20], mm4
    });

    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(step(), ExecResult::Continue);

    EXPECT_EQ(m_cpu->fpu.get_mm(7), 0x0506070832fb18e8ull);
    EXPECT_EQ(m_cpu->fpu.get_mm(1), 0x0002aaef3377bbfcull);
    EXPECT_EQ(m_cpu->fpu.get_mm(5), 0xf00ff00f0ff00ff0ull);

    auto stored = m_memory->read_u64(0x11020);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0xaabbccddeeff0011ull);
    EXPECT_EQ(m_cpu->rip, 0x1012u);
}

// Regression: movd eax, mm1 (0F 7E C8) was writing to GPR[8] instead of EAX
// because set_rm_packed applied mmx_pack_reg to the GPR rm field.
TEST(MmxDecoder, MovdRegMm_DecodesGprCorrectly) {
    // movd eax, mm1 = 0F 7E C8 (reg=1=mm1, rm=0=eax)
    auto inst = decode({0x0F, 0x7E, 0xC8});
    EXPECT_EQ(inst.opcode, Opcode::MOVD_RM32_MM);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX, not R8D
    EXPECT_EQ(inst.operands[1].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[1].reg, 1);  // MM1
}

TEST(MmxDecoder, MovdMmReg_DecodesGprCorrectly) {
    // movd mm0, ecx = 0F 6E C1 (reg=0=mm0, rm=1=ecx)
    auto inst = decode({0x0F, 0x6E, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::MOVD_MM_RM32);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[0].reg, 0);  // MM0
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 1);  // ECX, not R9D
}

// End-to-end: movd eax, mm1 writes low 32 bits of mm1 to EAX
TEST_F(MmxTest, MovdEaxMm1_WritesToEax) {
    m_cpu->fpu.set_mm(1, 0xDEADBEEF12345678ull);
    m_cpu->gpr.set32(0, 0xAAAAAAAA);  // EAX = sentinel

    // movd eax, mm1 = 0F 7E C8
    write_code({0x0F, 0x7E, 0xC8});
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(0), 0x12345678u);  // EAX = low 32 bits of mm1
}

TEST(MmxDecoder, PsrlqMmMm) {
    auto inst = decode({0x0F, 0xD3, 0xC1});  // psrlq mm0, mm1
    EXPECT_EQ(inst.opcode, Opcode::PSRLQ_MM_MMM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[1].reg, 1);
}

TEST_F(MmxTest, PsrlqUsesUnsignedQwordCount) {
    m_cpu->fpu.set_mm(0, 0x025F7FF7F2DF7FFFull);
    m_cpu->fpu.set_mm(1, 2);
    write_code({0x0F, 0xD3, 0xC1});  // psrlq mm0, mm1

    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0), 0x0097DFFDFCB7DFFFull);
}

TEST(MmxDecoder, PsllqMmMm) {
    auto inst = decode({0x0F, 0xF3, 0xC1});  // psllq mm0, mm1
    EXPECT_EQ(inst.opcode, Opcode::PSLLQ_MM_MMM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[1].reg, 1);
}

TEST_F(MmxTest, PsllqUsesUnsignedQwordCount) {
    m_cpu->fpu.set_mm(0, 0x0123456789ABCDEFull);
    m_cpu->fpu.set_mm(1, 4);
    write_code({0x0F, 0xF3, 0xC1});  // psllq mm0, mm1

    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0), 0x123456789ABCDEF0ull);
}

// End-to-end: bn_mul_words MMX loop pattern with w=0
TEST_F(MmxTest, BnMulWordsPattern_WZero) {
    // Set up memory: ap[0]=0xFFFFFFFF, ap[1]=0xFFFFFFFF
    u64 ap_addr = 0x2000;
    u64 rp_addr = 0x3000;
    ASSERT_TRUE(m_memory->write_u32(ap_addr, 0xFFFFFFFF));
    ASSERT_TRUE(m_memory->write_u32(ap_addr + 4, 0xFFFFFFFF));
    // Initialize rp with garbage
    ASSERT_TRUE(m_memory->write_u32(rp_addr, 0xBADBADBA));
    ASSERT_TRUE(m_memory->write_u32(rp_addr + 4, 0xBADBADBA));

    m_cpu->gpr.set32(0, static_cast<u32>(rp_addr));  // EAX = rp
    m_cpu->gpr.set32(2, static_cast<u32>(ap_addr));   // EDX = ap
    m_cpu->gpr.set32(1, 2);                           // ECX = num = 2

    // Store w=0 at [esp+0x10]
    u32 esp = m_cpu->gpr.get32(static_cast<u8>(RegIndex::RSP));
    ASSERT_TRUE(m_memory->write_u32(esp + 0x10, 0));

    write_code({
        0x0F, 0x6E, 0x44, 0x24, 0x10,  // movd mm0, [esp+0x10]  ; mm0 = w
        0x0F, 0xEF, 0xC9,              // pxor mm1, mm1          ; mm1 = 0
        // Iteration 1:
        0x0F, 0x6E, 0x12,              // movd mm2, [edx]        ; mm2 = ap[0]
        0x0F, 0xF4, 0xD0,              // pmuludq mm2, mm0       ; mm2 *= w
        0x0F, 0xD4, 0xCA,              // paddq mm1, mm2         ; accumulate
        0x0F, 0x7E, 0x08,              // movd [eax], mm1        ; store rp[0]
        0x0F, 0x73, 0xD1, 0x20,        // psrlq mm1, 32          ; carry
        // Iteration 2:
        0x0F, 0x6E, 0x52, 0x04,        // movd mm2, [edx+4]     ; mm2 = ap[1]
        0x0F, 0xF4, 0xD0,              // pmuludq mm2, mm0       ; mm2 *= w
        0x0F, 0xD4, 0xCA,              // paddq mm1, mm2         ; accumulate
        0x0F, 0x7E, 0x48, 0x04,        // movd [eax+4], mm1     ; store rp[1]
        0x0F, 0x73, 0xD1, 0x20,        // psrlq mm1, 32          ; carry
        0x0F, 0x7E, 0xC8,              // movd eax, mm1          ; return carry in EAX
        0x0F, 0x77,                     // emms
    });

    // Execute all instructions
    for (int i = 0; i < 13; ++i) {
        ASSERT_EQ(step(), ExecResult::Continue) << "Failed at step " << i;
    }

    // Verify: w=0, so all products are 0, carry is 0
    auto rp0 = m_memory->read_u32(rp_addr);
    auto rp1 = m_memory->read_u32(rp_addr + 4);
    ASSERT_TRUE(rp0.is_ok());
    ASSERT_TRUE(rp1.is_ok());
    EXPECT_EQ(rp0.value(), 0u);
    EXPECT_EQ(rp1.value(), 0u);
    EXPECT_EQ(m_cpu->gpr.get32(0), 0u);  // EAX = carry = 0
}
// ============================================================================
// PADDD mm, mm/m64 — per-lane dword add on the MM register file.
// The MMX-encoded form must land in MM0-7 (x87-aliased), not the XMM alias,
// or a guest that mixes MOVQ/CVTPI2PS with PADDD computes garbage.
// ============================================================================
TEST_F(MmxTest, PadddMmWrapsPerLane) {
    m_cpu->fpu.set_mm(0, (static_cast<u64>(0x7FFFFFFFu) << 32) | 0xFFFFFFFFu);
    m_cpu->fpu.set_mm(1, (static_cast<u64>(1u) << 32) | 2u);

    write_code({0x0F, 0xFE, 0xC1});  // PADDD mm0, mm1

    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(static_cast<u32>(m_cpu->fpu.get_mm(0)), 1u);            // wraps
    EXPECT_EQ(static_cast<u32>(m_cpu->fpu.get_mm(0) >> 32), 0x80000000u);
}

// ============================================================================
// PADDW / PACKUSWB / PSRAD on the MM register file (0F FD, 0F 67, 0F 72 /4).
// ============================================================================
TEST_F(MmxTest, PaddwMmPerLane) {
    m_cpu->fpu.set_mm(0, 0xFFFF0001'00020003ull);
    m_cpu->fpu.set_mm(1, 0x00010001'00010001ull);

    write_code({0x0F, 0xFD, 0xC1});  // PADDW mm0, mm1

    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0), 0x00000002'00030004ull);
}

TEST_F(MmxTest, PackuswbMmSaturates) {
    // dst words → low 4 bytes, src words → high 4 bytes; clamp to [0, 255].
    m_cpu->fpu.set_mm(0, 0x0100'FFFF'00FF'0007ull);  // 256, -1, 255, 7
    m_cpu->fpu.set_mm(1, 0x0000'0001'0002'0003ull);

    write_code({0x0F, 0x67, 0xC1});  // PACKUSWB mm0, mm1

    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0), 0x00010203'FF00FF07ull);
}

TEST_F(MmxTest, PsradMmShiftsSigned) {
    m_cpu->fpu.set_mm(2, 0xFFFFFFF0'00000010ull);  // -16, 16

    write_code({0x0F, 0x72, 0xE2, 0x02});  // PSRAD mm2, 2

    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(2), 0xFFFFFFFC'00000004ull);
}

TEST(MmxDecoder, PaddbPacksswbPmaddwdPmovmskbPsrlw) {
    auto paddb = decode({0x0F, 0xFC, 0xC1});
    EXPECT_EQ(paddb.opcode, Opcode::PADDB_MM_MMM64);
    EXPECT_EQ(paddb.operands[0].type, OperandType::RegMM);
    EXPECT_EQ(paddb.operands[0].reg, 0);
    EXPECT_EQ(paddb.operands[1].type, OperandType::RegMM);
    EXPECT_EQ(paddb.operands[1].reg, 1);

    auto pack = decode({0x0F, 0x63, 0xC1});
    EXPECT_EQ(pack.opcode, Opcode::PACKSSWB_MM_MMM64);

    auto madd = decode({0x0F, 0xF5, 0xC1});
    EXPECT_EQ(madd.opcode, Opcode::PMADDWD_MM_MMM64);

    auto ms = decode({0x0F, 0xD7, 0xC0});
    EXPECT_EQ(ms.opcode, Opcode::PMOVMSKB_R32_MM);
    EXPECT_EQ(ms.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(ms.operands[1].type, OperandType::RegMM);

    auto sh = decode({0x0F, 0x71, 0xD0, 0x01});
    EXPECT_EQ(sh.opcode, Opcode::PSRLW_MM_IMM8);
}

TEST_F(MmxTest, PaddbWrapsPerLane) {
    m_cpu->fpu.set_mm(0, 0xFF010080ull);
    m_cpu->fpu.set_mm(1, 0x0101FF01ull);
    write_code({0x0F, 0xFC, 0xC1});  // PADDB mm0, mm1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0) & 0xFFFFFFFFull, 0x0002FF81ull);
}

TEST_F(MmxTest, PacksswbSaturates) {
    m_cpu->fpu.set_mm(0, 0x0100'FF80'007F'0007ull);  // 256, -128, 127, 7
    m_cpu->fpu.set_mm(1, 0x7FFF'8000'0000'0001ull);
    write_code({0x0F, 0x63, 0xC1});  // PACKSSWB mm0, mm1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0), 0x7F800001'7F807F07ull);
}

TEST_F(MmxTest, PmaddwdAdjacentWords) {
    m_cpu->fpu.set_mm(0, 0x00020003'00010002ull);
    m_cpu->fpu.set_mm(1, 0x00040005'00030004ull);
    write_code({0x0F, 0xF5, 0xC1});  // PMADDWD mm0, mm1
    EXPECT_EQ(step(), ExecResult::Continue);
    // lo: 2*4 + 1*3 = 11; hi: 3*5 + 2*4 = 23
    EXPECT_EQ(m_cpu->fpu.get_mm(0), (23ull << 32) | 11ull);
}

TEST_F(MmxTest, PmovmskbExtractsByteSigns) {
    m_cpu->fpu.set_mm(0, 0x80'00'80'7F'FF'01'00'80ull);
    m_cpu->gpr.set32(0, 0xFFFFFFFFu);
    write_code({0x0F, 0xD7, 0xC0});  // PMOVMSKB eax, mm0
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(0), 0xA9u);
}

TEST_F(MmxTest, PsrlwImmZerosHighCount) {
    m_cpu->fpu.set_mm(0, 0x8000'0004'0002'0001ull);
    write_code({0x0F, 0x71, 0xD0, 0x01});  // PSRLW mm0, 1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0), 0x4000'0002'0001'0000ull);
}

TEST_F(MmxTest, PaddusbSaturates) {
    m_cpu->fpu.set_mm(0, 0xF0ull);
    m_cpu->fpu.set_mm(1, 0x20ull);
    write_code({0x0F, 0xDC, 0xC1});  // PADDUSB mm0, mm1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0) & 0xFFull, 0xFFull);
}

TEST(MmxDecoder, PinsrwMmEaxImm) {
    auto inst = decode({0x0F, 0xC4, 0xC0, 0x01});
    EXPECT_EQ(inst.opcode, Opcode::PINSRW_MM_R32M16_IMM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 0);
    EXPECT_EQ(inst.operands[2].imm, 1);
}

TEST(MmxDecoder, PextrwEaxMmImm) {
    auto inst = decode({0x0F, 0xC5, 0xC0, 0x01});
    EXPECT_EQ(inst.opcode, Opcode::PEXTRW_R32_MM_IMM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[1].reg, 0);
}

TEST(MmxDecoder, PsllwMmMm) {
    auto inst = decode({0x0F, 0xF1, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::PSLLW_MM_MMM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[1].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[1].reg, 1);
}

TEST(MmxDecoder, MovntqM64Mm) {
    auto inst = decode({0x0F, 0xE7, 0x07});
    EXPECT_EQ(inst.opcode, Opcode::MOVNTQ_M64_MM);
    EXPECT_EQ(inst.operands[0].type, OperandType::Mem64);
    EXPECT_EQ(inst.operands[1].type, OperandType::RegMM);
}

TEST(MmxDecoder, MaskmovqMmMm) {
    auto inst = decode({0x0F, 0xF7, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::MASKMOVQ_MM_MM);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[1].reg, 1);
}

TEST(MmxDecoder, MovntdqM128Xmm) {
    auto inst = decode({0x66, 0x0F, 0xE7, 0x07});
    EXPECT_EQ(inst.opcode, Opcode::MOVNTDQ_M128_XMM);
    EXPECT_EQ(inst.operands[0].type, OperandType::Mem128);
}

TEST(MmxDecoder, MaskmovdquXmmXmm) {
    auto inst = decode({0x66, 0x0F, 0xF7, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::MASKMOVDQU_XMM_XMM);
}

TEST(MmxDecoder, MovntqRegisterDestIsUd) {
    std::vector<u8> data{0x0F, 0xE7, 0xC0};
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    EXPECT_EQ(decode_instruction(reader, DecodeMode::Mode32, inst),
              DecodeError::InvalidOpcode);
}

TEST(MmxDecoder, F3MovntqIsUd) {
    std::vector<u8> data{0xF3, 0x0F, 0xE7, 0x07};
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    EXPECT_EQ(decode_instruction(reader, DecodeMode::Mode32, inst),
              DecodeError::InvalidOpcode);
}

TEST_F(MmxTest, PinsrwInsertsWordUsingImmLow2) {
    m_cpu->fpu.set_mm(0, 0x00000000AAAAull);
    m_cpu->gpr.set32(0, 0x1111BBBBu);  // EAX: low word BBBB
    // imm8=5 → MMX SEL := imm8[1:0] = 1, not XMM's [2:0]=5
    write_code({0x0F, 0xC4, 0xC0, 0x05});
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0), 0x00000000BBBBAAAAull);
}

TEST_F(MmxTest, PextrwExtractsWordUsingImmLow2) {
    m_cpu->fpu.set_mm(0, 0x0000CCCCDDDDull);
    m_cpu->gpr.set32(0, 0xFFFFFFFFu);
    write_code({0x0F, 0xC5, 0xC0, 0x05});  // SEL := 5 & 3 = 1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(0), 0xCCCCu);
}

TEST_F(MmxTest, PsllwRegCountShiftsWords) {
    m_cpu->fpu.set_mm(0, 0x8000000400020001ull);
    m_cpu->fpu.set_mm(1, 1ull);
    write_code({0x0F, 0xF1, 0xC1});  // PSLLW mm0, mm1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0), 0x0000000800040002ull);
}

TEST_F(MmxTest, PsllwRegCountAbove15Zeros) {
    m_cpu->fpu.set_mm(0, 0x8000000400020001ull);
    m_cpu->fpu.set_mm(1, 16ull);
    write_code({0x0F, 0xF1, 0xC1});
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0), 0ull);
}

TEST_F(MmxTest, PsradRegCountSignFills) {
    m_cpu->fpu.set_mm(0, 0x8000000000000001ull);
    m_cpu->fpu.set_mm(1, 32ull);
    write_code({0x0F, 0xE2, 0xC1});  // PSRAD mm0, mm1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0), 0xFFFFFFFF00000000ull);
}

TEST_F(MmxTest, MovntqStoresMmToMemory) {
    m_cpu->fpu.set_mm(0, 0x1122334455667788ull);
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RDI), 0x2000);
    write_code({0x0F, 0xE7, 0x07});  // MOVNTQ [edi], mm0
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_memory->read_u64(0x2000).value(), 0x1122334455667788ull);
}

TEST_F(MmxTest, MaskmovqStoresSelectedBytesToDsEdi) {
    m_cpu->fpu.set_mm(0, 0x0807060504030201ull);
    m_cpu->fpu.set_mm(1, 0x0080008000800080ull);  // write bytes 0,2,4,6
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::RDI), 0x2000);
    for (u64 i = 0; i < 8; ++i)
        ASSERT_TRUE(m_memory->write_u8(0x2000 + i, 0xFF));
    write_code({0x0F, 0xF7, 0xC1});  // MASKMOVQ mm0, mm1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_memory->read_u8(0x2000).value(), 0x01);
    EXPECT_EQ(m_memory->read_u8(0x2001).value(), 0xFF);
    EXPECT_EQ(m_memory->read_u8(0x2002).value(), 0x03);
    EXPECT_EQ(m_memory->read_u8(0x2003).value(), 0xFF);
    EXPECT_EQ(m_memory->read_u8(0x2004).value(), 0x05);
    EXPECT_EQ(m_memory->read_u8(0x2005).value(), 0xFF);
    EXPECT_EQ(m_memory->read_u8(0x2006).value(), 0x07);
    EXPECT_EQ(m_memory->read_u8(0x2007).value(), 0xFF);
    EXPECT_EQ(m_cpu->gpr.get32(static_cast<u8>(RegIndex::RDI)), 0x2000u);
}

TEST(MmxDecoder, Movq2dqXmmMm) {
    auto inst = decode({0xF3, 0x0F, 0xD6, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::MOVQ2DQ_XMM_MM);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg128);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[1].reg, 1);
}

TEST(MmxDecoder, Movdq2qMmXmm) {
    auto inst = decode({0xF2, 0x0F, 0xD6, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::MOVDQ2Q_MM_XMM);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[0].reg, 0);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg128);
    EXPECT_EQ(inst.operands[1].reg, 1);
}

TEST(MmxDecoder, Np0FD6IsUd) {
    std::vector<u8> data{0x0F, 0xD6, 0xC1};
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    EXPECT_EQ(decode_instruction(reader, DecodeMode::Mode32, inst),
              DecodeError::InvalidOpcode);
}

TEST_F(MmxTest, Movq2dqCopiesMmAndZerosHighXmm) {
    m_cpu->fpu.set_mm(1, 0x1122334455667788ull);
    m_cpu->sse.xmm[0].u64_[0] = 0xAAAAAAAAAAAAAAAAULL;
    m_cpu->sse.xmm[0].u64_[1] = 0xBBBBBBBBBBBBBBBBULL;
    write_code({0xF3, 0x0F, 0xD6, 0xC1});  // MOVQ2DQ xmm0, mm1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[0], 0x1122334455667788ull);
    EXPECT_EQ(m_cpu->sse.xmm[0].u64_[1], 0ull);
    EXPECT_EQ(m_cpu->fpu.get_mm(1), 0x1122334455667788ull);
}

TEST_F(MmxTest, Movdq2qCopiesLowXmmToMm) {
    m_cpu->sse.xmm[1].u64_[0] = 0x0123456789ABCDEFULL;
    m_cpu->sse.xmm[1].u64_[1] = 0xFEDCBA9876543210ULL;
    m_cpu->fpu.set_mm(0, 0xFFFFFFFFFFFFFFFFULL);
    write_code({0xF2, 0x0F, 0xD6, 0xC1});  // MOVDQ2Q mm0, xmm1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0), 0x0123456789ABCDEFULL);
    EXPECT_EQ(m_cpu->sse.xmm[1].u64_[1], 0xFEDCBA9876543210ULL);
}

// ---------------------------------------------------------------------------
// SSSE3 on MMX (NP 0F 38 / 0F 3A) and SSE4.1 PEXTRW m16.
// ---------------------------------------------------------------------------

TEST(MmxDecoder, PshufbMmMm) {
    auto inst = decode({0x0F, 0x38, 0x00, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::PSHUFB_MM_MMM64);
    EXPECT_EQ(inst.operands[0].type, OperandType::RegMM);
    EXPECT_EQ(inst.operands[1].type, OperandType::RegMM);
}

TEST(MmxDecoder, PshufbXmmStillRequires66) {
    auto inst = decode({0x66, 0x0F, 0x38, 0x00, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::PSHUFB_XMM_XMMM128);
}

TEST_F(MmxTest, PshufbPermutesBytesAndZerosOnBit7) {
    m_cpu->fpu.set_mm(0, 0x0706050403020100ull);
    m_cpu->fpu.set_mm(1, 0x80000701ull);  // bytes: 01, 07, 00, 80
    write_code({0x0F, 0x38, 0x00, 0xC1});  // PSHUFB mm0, mm1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0), 0x0000000000000701ull);
}

TEST_F(MmxTest, PalignrShiftsConcatenatedSrcDstByImm) {
    m_cpu->fpu.set_mm(0, 0x0807060504030201ull);
    m_cpu->fpu.set_mm(1, 0x100F0E0D0C0B0A09ull);
    write_code({0x0F, 0x3A, 0x0F, 0xC1, 0x02});  // PALIGNR mm0, mm1, 2
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0), 0x0201100F0E0D0C0Bull);
}

TEST_F(MmxTest, PabsbTakesAbsoluteValueIncludingMinInt) {
    m_cpu->fpu.set_mm(0, 0ull);
    m_cpu->fpu.set_mm(1, 0x80FF7F01ull);  // bytes: 01, 7F, FF, 80
    write_code({0x0F, 0x38, 0x1C, 0xC1});  // PABSB mm0, mm1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0), 0x80017F01ull);
}

TEST_F(MmxTest, PhaddwPairwiseAddsWords) {
    m_cpu->fpu.set_mm(0, 0x0004000300020001ull);
    m_cpu->fpu.set_mm(1, 0x0008000700060005ull);
    write_code({0x0F, 0x38, 0x01, 0xC1});  // PHADDW mm0, mm1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->fpu.get_mm(0), 0x000F000B00070003ull);
}

TEST(MmxDecoder, PextrwM16) {
    auto inst = decode({0x66, 0x0F, 0x3A, 0x15, 0x00, 0x01});
    EXPECT_EQ(inst.opcode, Opcode::PEXTRW_R32_XMM_IMM8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Mem16);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg128);
    EXPECT_EQ(inst.operands[1].reg, 0);
    EXPECT_EQ(inst.operands[2].imm, 1u);
}

TEST_F(MmxTest, PextrwWritesWordToMemory) {
    m_cpu->sse.xmm[0].u16_[0] = 0x1111;
    m_cpu->sse.xmm[0].u16_[1] = 0xABCD;
    m_cpu->gpr.set32(static_cast<u8>(RegIndex::EAX), 0x2000);
    write_code({0x66, 0x0F, 0x3A, 0x15, 0x00, 0x01});  // PEXTRW [eax], xmm0, 1
    EXPECT_EQ(step(), ExecResult::Continue);
    EXPECT_EQ(m_memory->read_u16(0x2000).value(), 0xABCDu);
}

// ============================================================================
// JIT vs interpreter parity for native MMX packed-op lowering.
// ============================================================================

class MmxJitTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(0x1000, 0x10000,
                                         MemoryProtection::ReadWrite).is_ok());
    }

    void init_cpu(CpuState& cpu) {
        cpu.init_protected_mode();
        cpu.gpr.set32(static_cast<u8>(RegIndex::ESP), 0x10000);
        cpu.rip = 0x1000;
    }

    BasicBlock build_block(const u8* code, usize len) {
        BasicBlock block;
        block.start_addr = 0x1000;
        MemoryByteReader reader(code, len);
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            if (decode_instruction(reader, DecodeMode::Mode32, inst)
                != DecodeError::Success) {
                break;
            }
            block.instructions.push_back(inst);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count = static_cast<u32>(block.instructions.size());
        return block;
    }

    void check_mmx_parity(std::initializer_list<u8> bytes,
                          std::function<void(CpuState&)> setup) {
        std::vector<u8> code(bytes);
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));

        std::vector<u8> initial_mem(0x10000);
        ASSERT_TRUE(m_memory->read_bytes(0x1000, initial_mem.data(),
                                         initial_mem.size()));

        BasicBlock block = build_block(code.data(), code.size());
        ASSERT_GT(block.instruction_count, 0u);

        CpuState icpu;
        init_cpu(icpu);
        setup(icpu);
        {
            Interpreter interp(icpu, *m_memory);
            EXPECT_EQ(interp.run(block.instruction_count), ExecResult::Continue);
        }

        ASSERT_TRUE(m_memory->write_bytes(0x1000, initial_mem.data(),
                                          initial_mem.size()));

        CpuState jcpu;
        init_cpu(jcpu);
        setup(jcpu);
        JitCompiler jit(*m_memory);
        CompiledBlock* cb = jit.compile(block);
        ASSERT_NE(cb, nullptr);
        EXPECT_EQ(jit.execute(*cb, jcpu), JitExecResult::Continue);

        for (u8 r = 0; r < 8; ++r) {
            EXPECT_EQ(icpu.fpu.get_mm(r), jcpu.fpu.get_mm(r))
                << "MM" << static_cast<int>(r) << " diverged";
            EXPECT_EQ(icpu.fpu.st[r].exponent_sign, jcpu.fpu.st[r].exponent_sign)
                << "MM" << static_cast<int>(r) << " exponent_sign diverged";
            EXPECT_EQ(icpu.gpr.get32(r), jcpu.gpr.get32(r))
                << "R" << static_cast<int>(r) << " diverged";
        }
        EXPECT_EQ(static_cast<u32>(icpu.rip), static_cast<u32>(jcpu.rip));
    }

    std::unique_ptr<GuestMemory> m_memory;
};

TEST_F(MmxJitTest, PadddMmMm) {
    check_mmx_parity({0x0F, 0xFE, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, (static_cast<u64>(0x7FFFFFFFu) << 32) | 0xFFFFFFFFu);
        cpu.fpu.set_mm(1, (static_cast<u64>(1u) << 32) | 2u);
    });
}

TEST_F(MmxJitTest, PxorMmMm) {
    check_mmx_parity({0x0F, 0xEF, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x0123456789ABCDEFULL);
        cpu.fpu.set_mm(1, 0xF0F0F0F0F0F0F0F0ULL);
    });
}

TEST_F(MmxJitTest, PcmpeqdMmMm) {
    check_mmx_parity({0x0F, 0x76, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0xAABBCCDD11223344ULL);
        cpu.fpu.set_mm(1, 0xAABBCCDD55667788ULL);
    });
}

TEST_F(MmxJitTest, PunpckldqMmMm) {
    check_mmx_parity({0x0F, 0x62, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x1122334455667788ULL);
        cpu.fpu.set_mm(1, 0x99AABBCCDDEEFF00ULL);
    });
}

TEST_F(MmxJitTest, PunpckhqdqMmMm) {
    check_mmx_parity({0x0F, 0x6D, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x1122334455667788ULL);
        cpu.fpu.set_mm(1, 0x99AABBCCDDEEFF00ULL);
    });
}

TEST_F(MmxJitTest, PandMmMem) {
    check_mmx_parity({0x0F, 0xDB, 0x00}, [&](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0xFFFFFFFF0F0F0F0FULL);
        cpu.gpr.set32(static_cast<u8>(RegIndex::EAX), 0x2000);
        ASSERT_TRUE(m_memory->write_u64(0x2000, 0x0F0F0F0FF0F0F0F0ULL));
    });
}

TEST_F(MmxJitTest, PackuswbMmMm) {
    check_mmx_parity({0x0F, 0x67, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x0100FFFF00FF0007ULL);
        cpu.fpu.set_mm(1, 0x0000000100020003ULL);
    });
}

TEST_F(MmxJitTest, PunpckhbwMmMm) {
    check_mmx_parity({0x0F, 0x68, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x1122334455667788ULL);
        cpu.fpu.set_mm(1, 0x99AABBCCDDEEFF00ULL);
    });
}

TEST_F(MmxJitTest, PsradImmAndPsllqReg) {
    check_mmx_parity({0x0F, 0x72, 0xE0, 0x02}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0xFFFFFFF000000010ULL);
    });
    check_mmx_parity({0x0F, 0xF3, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x0123456789ABCDEFULL);
        cpu.fpu.set_mm(1, 4ULL);
    });
    check_mmx_parity({0x0F, 0xD3, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x0123456789ABCDEFULL);
        cpu.fpu.set_mm(1, 64ULL);
    });
}

TEST_F(MmxJitTest, PshufwSwapsAdjacentWords) {
    check_mmx_parity({0x0F, 0x70, 0xC0, 0xB1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x0004000300020001ULL);
    });
}

TEST_F(MmxJitTest, PmaxswPminswSignedWords) {
    check_mmx_parity({0x0F, 0xEE, 0xC2}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x0002FFFF00010000ULL);
        cpu.fpu.set_mm(2, 0x0003FFFE00000005ULL);
    });
    check_mmx_parity({0x0F, 0xEA, 0xCB}, [](CpuState& cpu) {
        cpu.fpu.set_mm(1, 0x0002FFFF00010000ULL);
        cpu.fpu.set_mm(3, 0x0003FFFE00000005ULL);
    });
}

TEST_F(MmxJitTest, PackssdwSaturatesToInt16) {
    check_mmx_parity({0x0F, 0x6B, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x000080000000007FULL);
        cpu.fpu.set_mm(1, 0xFFFF8000FFFFFFFFULL);
    });
}

TEST_F(MmxJitTest, PacksswbSaturates) {
    check_mmx_parity({0x0F, 0x63, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x0100FF80007F0007ULL);
        cpu.fpu.set_mm(1, 0x7FFF800000000001ULL);
    });
}

TEST_F(MmxJitTest, PaddusbSaturates) {
    check_mmx_parity({0x0F, 0xDC, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0xF0ULL);
        cpu.fpu.set_mm(1, 0x20ULL);
    });
}

TEST_F(MmxJitTest, PmaddwdAdjacentWords) {
    check_mmx_parity({0x0F, 0xF5, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x0002000300010002ULL);
        cpu.fpu.set_mm(1, 0x0004000500030004ULL);
    });
    check_mmx_parity({0x0F, 0xF5, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x8000800080008000ULL);
        cpu.fpu.set_mm(1, 0x8000800080008000ULL);
    });
}

TEST_F(MmxJitTest, PmovmskbExtractsByteSigns) {
    check_mmx_parity({0x0F, 0xD7, 0xC0}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x8000807FFF010080ULL);
        cpu.gpr.set32(0, 0xFFFFFFFFu);
    });
}

TEST_F(MmxJitTest, PinsrwInsertsWordUsingImmLow2) {
    check_mmx_parity({0x0F, 0xC4, 0xC0, 0x05}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x00000000AAAAULL);
        cpu.gpr.set32(0, 0x1111BBBBu);
    });
}

TEST_F(MmxJitTest, PinsrwInsertsWordFromMemory) {
    check_mmx_parity({0x0F, 0xC4, 0x00, 0x01}, [&](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x00000000AAAAull);
        cpu.gpr.set32(static_cast<u8>(RegIndex::EAX), 0x2000);
        ASSERT_TRUE(m_memory->write_u16(0x2000, 0xBBBB));
    });
}

TEST_F(MmxJitTest, PextrwExtractsWordUsingImmLow2) {
    check_mmx_parity({0x0F, 0xC5, 0xC0, 0x05}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x0000CCCCDDDDULL);
        cpu.gpr.set32(0, 0xFFFFFFFFu);
    });
}

TEST_F(MmxJitTest, PshufbPermutesBytesAndZerosOnBit7) {
    check_mmx_parity({0x0F, 0x38, 0x00, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x0706050403020100ULL);
        cpu.fpu.set_mm(1, 0x80000701ULL);
    });
    check_mmx_parity({0x0F, 0x38, 0x00, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x0706050403020100ULL);
        cpu.fpu.set_mm(1, 0x08ULL);
    });
}

TEST_F(MmxJitTest, PhaddwPairwiseAddsWords) {
    check_mmx_parity({0x0F, 0x38, 0x01, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x0004000300020001ULL);
        cpu.fpu.set_mm(1, 0x0008000700060005ULL);
    });
}

TEST_F(MmxJitTest, PabsbTakesAbsoluteValueIncludingMinInt) {
    check_mmx_parity({0x0F, 0x38, 0x1C, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0ULL);
        cpu.fpu.set_mm(1, 0x80FF7F01ULL);
    });
}

TEST_F(MmxJitTest, PalignrShiftsConcatenatedSrcDstByImm) {
    check_mmx_parity({0x0F, 0x3A, 0x0F, 0xC1, 0x02}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x0807060504030201ULL);
        cpu.fpu.set_mm(1, 0x100F0E0D0C0B0A09ULL);
    });
    check_mmx_parity({0x0F, 0x3A, 0x0F, 0xC1, 0x10}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x0807060504030201ULL);
        cpu.fpu.set_mm(1, 0x100F0E0D0C0B0A09ULL);
    });
}

TEST_F(MmxJitTest, MaskmovqStoresSelectedBytesToDsEdi) {
    const u8 code[] = {0x0F, 0xF7, 0xC1};  // MASKMOVQ mm0, mm1
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    for (u64 i = 0; i < 8; ++i)
        ASSERT_TRUE(m_memory->write_u8(0x2000 + i, 0xFF));

    std::vector<u8> initial_mem(0x10000);
    ASSERT_TRUE(m_memory->read_bytes(0x1000, initial_mem.data(),
                                     initial_mem.size()));

    auto setup = [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x0807060504030201ull);
        cpu.fpu.set_mm(1, 0x0080008000800080ull);  // write bytes 0,2,4,6
        cpu.gpr.set32(static_cast<u8>(RegIndex::RDI), 0x2000);
    };

    BasicBlock block = build_block(code, sizeof(code));
    ASSERT_GT(block.instruction_count, 0u);

    CpuState icpu;
    init_cpu(icpu);
    setup(icpu);
    {
        Interpreter interp(icpu, *m_memory);
        EXPECT_EQ(interp.run(block.instruction_count), ExecResult::Continue);
    }
    u8 interp_dest[8];
    ASSERT_TRUE(m_memory->read_bytes(0x2000, interp_dest, 8));

    ASSERT_TRUE(m_memory->write_bytes(0x1000, initial_mem.data(),
                                      initial_mem.size()));

    CpuState jcpu;
    init_cpu(jcpu);
    setup(jcpu);
    JitCompiler jit(*m_memory);
    CompiledBlock* cb = jit.compile(block);
    ASSERT_NE(cb, nullptr);
    EXPECT_EQ(jit.execute(*cb, jcpu), JitExecResult::Continue);

    u8 jit_dest[8];
    ASSERT_TRUE(m_memory->read_bytes(0x2000, jit_dest, 8));
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(interp_dest[i], jit_dest[i]) << "dest byte " << i;
    }
    EXPECT_EQ(icpu.fpu.get_mm(0), jcpu.fpu.get_mm(0));
    EXPECT_EQ(icpu.fpu.st[0].exponent_sign, jcpu.fpu.st[0].exponent_sign);
    EXPECT_EQ(icpu.gpr.get32(static_cast<u8>(RegIndex::RDI)), 0x2000u);
    EXPECT_EQ(jcpu.gpr.get32(static_cast<u8>(RegIndex::RDI)), 0x2000u);
}

TEST_F(MmxJitTest, PaddsbSaturatesSignedBytes) {
    check_mmx_parity({0x0F, 0xEC, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x7F7F80FFULL);
        cpu.fpu.set_mm(1, 0x0102FF01ULL);
    });
}

TEST_F(MmxJitTest, PsubusbSaturatesUnsignedBytes) {
    check_mmx_parity({0x0F, 0xD8, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x10F00500ULL);
        cpu.fpu.set_mm(1, 0x20E01001ULL);
    });
}

TEST_F(MmxJitTest, PmulhwSignedHighWords) {
    check_mmx_parity({0x0F, 0xE5, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x40007FFF8000FFFFULL);
        cpu.fpu.set_mm(1, 0x40007FFF80000002ULL);
    });
}

TEST_F(MmxJitTest, PminubUnsignedBytes) {
    check_mmx_parity({0x0F, 0xDA, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x01FF80A5ULL);
        cpu.fpu.set_mm(1, 0x02FE7FB4ULL);
    });
}

TEST_F(MmxJitTest, PhadddPairwiseAddsDwords) {
    check_mmx_parity({0x0F, 0x38, 0x02, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x0000000300000001ULL);
        cpu.fpu.set_mm(1, 0x0000000400000002ULL);
    });
}

TEST_F(MmxJitTest, PabswTakesAbsoluteValueIncludingMinInt) {
    check_mmx_parity({0x0F, 0x38, 0x1D, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0ULL);
        cpu.fpu.set_mm(1, 0x80007FFF0001FFFFULL);
    });
}

TEST_F(MmxJitTest, PsrlwImmShiftsWords) {
    check_mmx_parity({0x0F, 0x71, 0xD0, 0x04}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x8000400020001000ULL);
    });
    check_mmx_parity({0x0F, 0x71, 0xD0, 0x10}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x8000400020001000ULL);
    });
}

TEST_F(MmxJitTest, PsignbAppliesSourceSign) {
    check_mmx_parity({0x0F, 0x38, 0x08, 0xC1}, [](CpuState& cpu) {
        cpu.fpu.set_mm(0, 0x01FF7F80ULL);
        cpu.fpu.set_mm(1, 0xFF00017FULL);
    });
}
