// SPDX-License-Identifier: LGPL-2.1-only
// Intel SDM Vol. 2B PTEST, pp. 4-496/497 and Vol. 2A Table 2-21.
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include <vector>

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {
constexpr u64 arithmetic_flags = FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF;
std::vector<u8> encoding(unsigned form, bool memory, bool w = false, u8 vvvv = 0) {
    // SDM: legacy 66 0F 38 17 /r; VEX.128/256.66.0F38.WIG 17 /r.
    // VEX.vvvv is stored inverted by PrefixState (encoded 1111b => 0).
    if (form == 0) return {0x66, 0x0F, 0x38, 0x17, static_cast<u8>(memory ? 0x08 : 0xCA)};
    return {0xC4, 0xE2, static_cast<u8>((w ? 0x80 : 0) | ((~vvvv & 15) << 3) |
                         (form == 2 ? 4 : 0) | 1), 0x17,
            static_cast<u8>(memory ? 0x08 : 0xCA)}; // source1 xmm/ymm1, source2 xmm/ymm2 or [eax/rax]
}
void init(CpuState& cpu, bool x64) {
    if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
    cpu.rip = 0x1000;
    cpu.gpr.set64(0, 0x3000);
    cpu.gpr.set64(4, 0x20000);
    cpu.flags.rflags = 0x202 | arithmetic_flags;
    for (u8 r = 0; r < 16; ++r) {
        for (u8 b = 0; b < 16; ++b) {
            cpu.sse.xmm[r].u8_[b] = static_cast<u8>(r * 17 + b);
            cpu.avx.ymm_high[r].u8_[b] = static_cast<u8>(r * 13 + b + 1);
        }
    }
}
void expect_vectors_unchanged(const CpuState& after, const CpuState& before) {
    EXPECT_EQ(std::memcmp(after.sse.xmm, before.sse.xmm, sizeof(after.sse.xmm)), 0);
    EXPECT_EQ(std::memcmp(after.avx.ymm_high, before.avx.ymm_high, sizeof(after.avx.ymm_high)), 0);
}
}

TEST(VptestContract, FlagsReduceEveryBitAndPreserveAllVectorSourcesBothModesAndTiers) {
    for (bool x64 : {false, true}) for (unsigned form : {0u, 1u, 2u})
    for (bool memory : {false, true}) for (bool native : {false, true}) {
        GuestMemory mem;
        ASSERT_TRUE(mem.map_region(0x1000, 0x20000, MemoryProtection::ReadWriteExecute));
        const auto code = encoding(form, memory);
        ASSERT_TRUE(mem.write_bytes(0x1000, code.data(), code.size()));
        MemoryByteReader reader(code.data(), code.size());
        DecodedInstruction inst;
        ASSERT_EQ(decode_instruction(reader, x64 ? DecodeMode::Mode64 : DecodeMode::Mode32, inst), DecodeError::Success);
        BasicBlock block;
        block.start_addr = 0x1000; block.is_64bit = x64;
        block.byte_length = static_cast<u32>(code.size()); block.instruction_count = 1;
        block.instructions.push_back(inst);
        JitCompiler jit(mem);
        CompiledBlock* compiled = native ? jit.compile(block) : nullptr;
        if (native) ASSERT_NE(compiled, nullptr);
        const unsigned bits = form == 2 ? 256 : 128;
        for (unsigned bit = 0; bit < bits; ++bit) for (unsigned relation = 0; relation < 4; ++relation) {
            SCOPED_TRACE(::testing::Message() << "x64=" << x64 << " form=" << form << " mem=" << memory << " jit=" << native << " bit=" << bit << " relation=" << relation);
            CpuState cpu; init(cpu, x64);
            cpu.sse.xmm[1].clear(); cpu.sse.xmm[2].clear();
            if (form == 2) { cpu.avx.ymm_high[1].clear(); cpu.avx.ymm_high[2].clear(); }
            auto& dst = bit < 128 ? cpu.sse.xmm[1] : cpu.avx.ymm_high[1];
            auto& src = bit < 128 ? cpu.sse.xmm[2] : cpu.avx.ymm_high[2];
            // same-bit => ZF=0 CF=1; absent first source => ZF=1 CF=0;
            // absent second source => ZF=1 CF=1. SDM equations, no Vitreo oracle.
            if (relation != 1) dst.u8_[(bit % 128) / 8] = static_cast<u8>(1u << (bit % 8));
            if (relation != 2) src.u8_[(bit % 128) / 8] = static_cast<u8>(1u << (bit % 8));
            if (relation == 3) src.u8_[(bit % 128) / 8] |= static_cast<u8>(1u << ((bit + 1) % 8));
            std::array<XmmReg, 2> source{cpu.sse.xmm[2], cpu.avx.ymm_high[2]};
            if (memory) ASSERT_TRUE(mem.write_bytes(0x3000, reinterpret_cast<const u8*>(source.data()), bits / 8));
            const CpuState before = cpu;
            if (native) { EXPECT_EQ(jit.execute(*compiled, cpu), JitExecResult::Continue); EXPECT_FALSE(cpu.pending_raise.active); }
            else { Interpreter interp(cpu, mem); EXPECT_EQ(interp.step(), ExecResult::Continue); }
            cpu.materialize_lazy_flags();
            const u64 expected = (relation == 1 || relation == 2 ? FLAG_ZF : 0) |
                                 (relation == 0 || relation == 2 ? FLAG_CF : 0);
            EXPECT_EQ(cpu.flags.rflags & arithmetic_flags, expected);
            EXPECT_EQ(cpu.flags.rflags & ~arithmetic_flags, before.flags.rflags & ~arithmetic_flags);
            expect_vectors_unchanged(cpu, before);
            for (u8 reg = 0; reg < (x64 ? 16 : 8); ++reg)
                EXPECT_EQ(cpu.gpr.get64(reg), before.gpr.get64(reg));
        }
    }
}

TEST(VptestContract, MemoryAlignmentWidthAndFaultAreTransactionalBothModesAndTiers) {
    for (bool x64 : {false, true}) for (unsigned form : {0u, 1u, 2u})
    for (bool native : {false, true}) for (bool crosses_end : {false, true}) {
        GuestMemory mem;
        ASSERT_TRUE(mem.map_region(0x1000, 0x20000, MemoryProtection::ReadWriteExecute));
        const auto code = encoding(form, true, form != 0); // WIG=1 too
        ASSERT_TRUE(mem.write_bytes(0x1000, code.data(), code.size()));
        CpuState cpu; init(cpu, x64);
        const u64 address = crosses_end ? 0x20FF0 : 0x3001;
        cpu.gpr.set64(0, address);
        std::array<u8, 32> source{}; source[0] = 1;
        ASSERT_TRUE(mem.write_bytes(address, source.data(), crosses_end ? 16 : 32));
        cpu.sse.xmm[1].clear(); cpu.avx.ymm_high[1].clear();
        const CpuState before = cpu;
        const bool faults = crosses_end ? form == 2 : form == 0;
        if (native) {
            MemoryByteReader reader(code.data(), code.size()); DecodedInstruction inst;
            ASSERT_EQ(decode_instruction(reader, x64 ? DecodeMode::Mode64 : DecodeMode::Mode32, inst), DecodeError::Success);
            BasicBlock block; block.start_addr = 0x1000; block.is_64bit = x64;
            block.byte_length = static_cast<u32>(code.size()); block.instruction_count = 1; block.instructions.push_back(inst);
            JitCompiler jit(mem); auto* compiled = jit.compile(block); ASSERT_NE(compiled, nullptr);
            (void)jit.execute(*compiled, cpu);
            EXPECT_EQ(cpu.pending_raise.active, faults);
        } else { Interpreter interp(cpu, mem); EXPECT_EQ(interp.step(), faults ? ExecResult::Exception : ExecResult::Continue); }
        cpu.materialize_lazy_flags();
        EXPECT_EQ(cpu.flags.rflags, faults ? before.flags.rflags : ((before.flags.rflags & ~arithmetic_flags) | FLAG_ZF));
        EXPECT_EQ(cpu.rip, faults ? 0x1000u : 0x1000u + code.size());
        expect_vectors_unchanged(cpu, before);
    }
}

TEST(VptestContract, ReservedVvvvRaisesInvalidOpcodeForEveryEncodingBothModes) {
    for (bool x64 : {false, true}) for (unsigned form : {1u, 2u})
    for (bool memory : {false, true}) for (bool w : {false, true}) for (u8 vvvv = 0; vvvv < 16; ++vvvv) {
        const auto code = encoding(form, memory, w, vvvv);
        MemoryByteReader reader(code.data(), code.size()); DecodedInstruction inst;
        EXPECT_EQ(decode_instruction(reader, x64 ? DecodeMode::Mode64 : DecodeMode::Mode32, inst),
                  vvvv == 0 ? DecodeError::Success : DecodeError::InvalidOpcode);
    }
}
