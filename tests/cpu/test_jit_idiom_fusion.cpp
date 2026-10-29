// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT SSE idiom fusion tests (CPU-041)

#include <gtest/gtest.h>
#include <cstring>
#include <functional>
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/jit_helpers.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/idiom_detector.hpp"
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

constexpr u8 ESI = static_cast<u8>(RegIndex::RSI);
constexpr u8 EDI = static_cast<u8>(RegIndex::RDI);
constexpr u8 EAX = static_cast<u8>(RegIndex::RAX);
constexpr u8 ECX = static_cast<u8>(RegIndex::RCX);
constexpr u8 ESP = static_cast<u8>(RegIndex::RSP);

class IdiomFusionTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        auto map_result = m_memory->map_region(0x1000, 0x20000,
                                                MemoryProtection::ReadWrite);
        ASSERT_TRUE(map_result);
        m_jit = std::make_unique<JitCompiler>(*m_memory);
    }

    BasicBlock build_block(const u8* code, usize len) {
        BasicBlock block;
        block.start_addr = 0x1000;
        MemoryByteReader reader(code, len);
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            DecodeError err =
                decode_instruction(reader, DecodeMode::Mode32, inst);
            if (err != DecodeError::Success) break;
            block.instructions.push_back(inst);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count =
            static_cast<u32>(block.instructions.size());
        return block;
    }

    BasicBlock build_block64(const u8* code, usize len) {
        BasicBlock block;
        block.start_addr = 0x1000;
        block.is_64bit = true;
        MemoryByteReader reader(code, len);
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            DecodeError err =
                decode_instruction(reader, DecodeMode::Mode64, inst);
            if (err != DecodeError::Success) break;
            block.instructions.push_back(inst);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count =
            static_cast<u32>(block.instructions.size());
        return block;
    }

    void init_cpu(CpuState& cpu) {
        cpu.init_protected_mode();
        cpu.gpr.set32(ESP, 0x10000);
        cpu.rip = 0x1000;
    }

    void init_cpu64(CpuState& cpu) {
        cpu.init_long_mode();
        cpu.gpr.set64(ESP, 0x10000);
        cpu.rip = 0x1000;
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
};

// ============================================================================
// Idiom Detection Tests
// ============================================================================

TEST_F(IdiomFusionTest, DetectSseMemcpy2x16) {
    // 2 MOVDQU loads from [ESI+0], [ESI+16]
    // 2 MOVDQU stores to [EDI+0], [EDI+16]
    // F3 0F 6F 06          = MOVDQU XMM0, [ESI]
    // F3 0F 6F 4E 10       = MOVDQU XMM1, [ESI+16]
    // F3 0F 7F 07          = MOVDQU [EDI], XMM0
    // F3 0F 7F 4F 10       = MOVDQU [EDI+16], XMM1
    const u8 code[] = {
        0xF3, 0x0F, 0x6F, 0x06,
        0xF3, 0x0F, 0x6F, 0x4E, 0x10,
        0xF3, 0x0F, 0x7F, 0x07,
        0xF3, 0x0F, 0x7F, 0x4F, 0x10
    };
    auto block = build_block(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 4u);

    auto idioms = detect_idioms(block);
    ASSERT_EQ(idioms.size(), 1u);
    EXPECT_EQ(idioms[0].type, IdiomType::SseMemcpy);
    EXPECT_EQ(idioms[0].start_idx, 0u);
    EXPECT_EQ(idioms[0].end_idx, 4u);
    EXPECT_EQ(idioms[0].byte_count, 32u);
    EXPECT_EQ(idioms[0].src_base_reg, ESI);
    EXPECT_EQ(idioms[0].dst_base_reg, EDI);
}

TEST_F(IdiomFusionTest, NoIdiomForSingleLoad) {
    // Only 1 load + 1 store — too short to fuse
    const u8 code[] = {
        0xF3, 0x0F, 0x6F, 0x06,       // MOVDQU XMM0, [ESI]
        0xF3, 0x0F, 0x7F, 0x07        // MOVDQU [EDI], XMM0
    };
    auto block = build_block(code, sizeof(code));
    auto idioms = detect_idioms(block);
    EXPECT_TRUE(idioms.empty());
}

TEST_F(IdiomFusionTest, DetectScalarZeroFillLoop) {
    const u8 code[] = {
        0xC6, 0x00, 0x00,       // mov byte ptr [eax], 0
        0x8D, 0x40, 0x01,       // lea eax, [eax + 1]
        0x83, 0xE9, 0x01,       // sub ecx, 1
        0x75, 0xF5              // jne -11
    };
    auto block = build_block(code, sizeof(code));
    auto match = detect_zero_fill_loop(block);
    ASSERT_TRUE(match.has_value());
    EXPECT_EQ(match->dst_reg, EAX);
    EXPECT_EQ(match->count_reg, ECX);
    EXPECT_EQ(match->branch_offset, 9u);
    EXPECT_EQ(match->fallthrough_offset, 11u);
}

TEST_F(IdiomFusionTest, RejectScalarZeroFillWithoutBackEdge) {
    const u8 code[] = {
        0xC6, 0x00, 0x00,
        0x8D, 0x40, 0x01,
        0x83, 0xE9, 0x01,
        0x75, 0x00              // jne fallthrough, not the loop start
    };
    auto block = build_block(code, sizeof(code));
    EXPECT_FALSE(detect_zero_fill_loop(block).has_value());
}

TEST_F(IdiomFusionTest, ScalarZeroFillCompletesWithExactStateAndFlags) {
    const u8 code[] = {
        0xC6, 0x00, 0x00,
        0x8D, 0x40, 0x01,
        0x83, 0xE9, 0x01,
        0x75, 0xF5
    };
    std::array<u8, 64> initial;
    initial.fill(0xA5);
    ASSERT_TRUE(m_memory->write_bytes(
        0x3000, reinterpret_cast<const Byte*>(initial.data()), initial.size()));

    auto block = build_block(code, sizeof(code));
    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(EAX, 0x3008);
    cpu.gpr.set32(ECX, 37);
    auto* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);

    EXPECT_EQ(cpu.gpr.get32(EAX), 0x302Du);
    EXPECT_EQ(cpu.gpr.get32(ECX), 0u);
    EXPECT_EQ(cpu.rip, 0x100Bu);
    EXPECT_EQ(cpu.jit_iters, 37u);
    cpu.materialize_lazy_flags();
    EXPECT_TRUE(cpu.flags.zf());
    EXPECT_FALSE(cpu.flags.cf());
    EXPECT_FALSE(cpu.flags.sf());
    EXPECT_FALSE(cpu.flags.of());

    std::array<u8, 64> actual{};
    ASSERT_TRUE(m_memory->read_bytes(
        0x3000, reinterpret_cast<Byte*>(actual.data()), actual.size()));
    for (usize i = 0; i < actual.size(); ++i) {
        const u8 expected = (i >= 8 && i < 45) ? 0 : 0xA5;
        EXPECT_EQ(actual[i], expected) << "byte " << i;
    }
}

TEST_F(IdiomFusionTest, ScalarZeroFillHonorsJitLoopBudget) {
    const u8 code[] = {
        0xC6, 0x00, 0x00,
        0x8D, 0x40, 0x01,
        0x83, 0xE9, 0x01,
        0x75, 0xF5
    };
    constexpr u32 count = MAX_JIT_LOOP_ITERS + 17;
    std::vector<u8> initial(count + 16, 0x5A);
    ASSERT_TRUE(m_memory->write_bytes(
        0x3000, reinterpret_cast<const Byte*>(initial.data()), initial.size()));

    auto block = build_block(code, sizeof(code));
    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(EAX, 0x3000);
    cpu.gpr.set32(ECX, count);
    auto* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);

    EXPECT_EQ(cpu.gpr.get32(EAX), 0x3000u + MAX_JIT_LOOP_ITERS);
    EXPECT_EQ(cpu.gpr.get32(ECX), 17u);
    EXPECT_EQ(cpu.rip, 0x1009u);
    EXPECT_EQ(cpu.jit_iters, MAX_JIT_LOOP_ITERS);
    cpu.materialize_lazy_flags();
    EXPECT_FALSE(cpu.flags.zf());
    EXPECT_FALSE(cpu.flags.cf());

    std::array<u8, 18> tail{};
    ASSERT_TRUE(m_memory->read_bytes(
        0x3000 + MAX_JIT_LOOP_ITERS - 1,
        reinterpret_cast<Byte*>(tail.data()), tail.size()));
    EXPECT_EQ(tail[0], 0);
    for (usize i = 1; i < tail.size(); ++i) {
        EXPECT_EQ(tail[i], 0x5A) << "tail byte " << i;
    }
}

TEST_F(IdiomFusionTest, InBlockJccLoopReturnsAfterMaxInBlockIters) {
    // `inc eax; sub ecx,1; jnz` is not a fused stos idiom, so it uses the
    // in-block Jcc cap (not MAX_JIT_LOOP_ITERS). The cap is a dispatcher
    // safety valve for the 16 ms GUI pump, not a per-iteration budget.
    const u8 code[] = {
        0x40, 0x83, 0xE9, 0x01, 0x75, 0xFA
    };
    constexpr u32 count = MAX_IN_BLOCK_LOOP_ITERS + 17;
    auto block = build_block(code, sizeof(code));
    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(EAX, 0);
    cpu.gpr.set32(ECX, count);
    auto* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
    EXPECT_EQ(cpu.gpr.get32(EAX), MAX_IN_BLOCK_LOOP_ITERS);
    EXPECT_EQ(cpu.gpr.get32(ECX), 17u);
    EXPECT_EQ(cpu.jit_iters, MAX_IN_BLOCK_LOOP_ITERS);
}

TEST_F(IdiomFusionTest, InBlockLoopOpcodeReturnsAfterMaxInBlockIters) {
    const u8 code[] = {
        0x40,             // inc eax
        0xE2, 0xFD,       // loop 0x1000
    };
    constexpr u32 count = MAX_IN_BLOCK_LOOP_ITERS + 17;
    auto block = build_block(code, sizeof(code));
    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(EAX, 0);
    cpu.gpr.set32(ECX, count);
    auto* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
    EXPECT_EQ(cpu.gpr.get32(EAX), MAX_IN_BLOCK_LOOP_ITERS);
    EXPECT_EQ(cpu.gpr.get32(ECX), 17u);
    EXPECT_EQ(cpu.rip, 0x1000u);
    EXPECT_EQ(cpu.jit_iters, MAX_IN_BLOCK_LOOP_ITERS);
}

TEST_F(IdiomFusionTest, InBlockJccCapPublishesDeferredLazyFlags) {
    const u8 code[] = {
        0x83, 0xC0, 0x00, // add eax,0 (publisher and loop target)
        0x75, 0xFB,       // jnz 0x1000
        0x31, 0xC9,       // xor ecx,ecx (fall-through flag killer)
    };
    auto block = build_block(code, sizeof(code));
    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(EAX, 0x80000000u);
    auto* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
    EXPECT_EQ(cpu.rip, 0x1003u);
    EXPECT_EQ(cpu.jit_iters, MAX_IN_BLOCK_LOOP_ITERS);
    cpu.materialize_lazy_flags();
    EXPECT_FALSE(cpu.flags.cf());
    EXPECT_FALSE(cpu.flags.zf());
    EXPECT_TRUE(cpu.flags.sf());
    EXPECT_FALSE(cpu.flags.of());
    EXPECT_TRUE(cpu.flags.pf());
}

TEST_F(IdiomFusionTest, InBlockJccTargetAfterIndex32UsesFullPositionTable) {
    // The position table holds 64 instruction starts. Put the loop header at
    // index 33 so duplicate bookkeeping would exhaust the table before it and
    // make the taken JNZ return to the dispatcher after one iteration.
    std::vector<u8> code(33, 0x90); // nop
    code.insert(code.end(), {
        0x40,                         // inc eax (loop header)
        0x83, 0xE9, 0x01,             // sub ecx,1
        0x75, 0xFA,                   // jnz loop header
    });
    auto block = build_block(code.data(), code.size());
    ASSERT_EQ(block.instruction_count, 36u);

    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(EAX, 0);
    cpu.gpr.set32(ECX, 17);
    auto* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    if (m_jit->ir_emit_active())
        GTEST_SKIP() << "the in-block loop is the stencil path's; Tier 1 "
                        "leaves the block at the loop header";
    ASSERT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);

    EXPECT_EQ(cpu.gpr.get32(EAX), 17u);
    EXPECT_EQ(cpu.gpr.get32(ECX), 0u);
    EXPECT_EQ(cpu.rip, 0x1000u + code.size());
    EXPECT_EQ(cpu.jit_iters, 17u);
}

TEST_F(IdiomFusionTest, X64InBlockJccLoopToBlockStartMatchesInterpreter) {
    // inc rax; sub rcx,1; jnz -9. Intel SDM Vol. 2, Jcc: JNZ takes the
    // relative branch when ZF=0 and does not modify the SUB-owned flags.
    const u8 code[] = {
        0x48, 0xFF, 0xC0,
        0x48, 0x83, 0xE9, 0x01,
        0x75, 0xF7,
    };
    constexpr u64 count = 37;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    auto block = build_block64(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 3u);

    CpuState expected;
    init_cpu64(expected);
    expected.gpr.set64(EAX, 0x100000000ull);
    expected.gpr.set64(ECX, count);
    {
        Interpreter interpreter(expected, *m_memory);
        ASSERT_EQ(interpreter.run(static_cast<u32>(count * 3)),
                  ExecResult::Continue);
    }
    expected.materialize_lazy_flags();

    CpuState actual;
    init_cpu64(actual);
    actual.gpr.set64(EAX, 0x100000000ull);
    actual.gpr.set64(ECX, count);
    auto* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_EQ(m_jit->execute(*compiled, actual), JitExecResult::Continue);
    actual.materialize_lazy_flags();

    EXPECT_EQ(actual.gpr.get64(EAX), expected.gpr.get64(EAX));
    EXPECT_EQ(actual.gpr.get64(ECX), expected.gpr.get64(ECX));
    EXPECT_EQ(actual.rip, expected.rip);
    EXPECT_EQ(actual.jit_iters, count);
    EXPECT_EQ(actual.flags.cf(), expected.flags.cf());
    EXPECT_EQ(actual.flags.zf(), expected.flags.zf());
    EXPECT_EQ(actual.flags.sf(), expected.flags.sf());
    EXPECT_EQ(actual.flags.of(), expected.flags.of());
    EXPECT_EQ(actual.flags.pf(), expected.flags.pf());
}

TEST_F(IdiomFusionTest, X64BlockStartLoopDoesNotLeakCapFlagsIntoAdc) {
    // adc rax,0; add rcx,-1; jnz start. Intel SDM Vol. 2 defines ADC as
    // consuming architectural CF and Jcc as preserving flags. The JIT's host
    // loop-budget SUBS must therefore never become the next iteration's CF.
    const u8 code[] = {
        0x48, 0x83, 0xD0, 0x00,
        0x48, 0x83, 0xC1, 0xFF,
        0x75, 0xF6,
    };
    constexpr u64 count = 5;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    auto block = build_block64(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 3u);

    CpuState expected;
    init_cpu64(expected);
    expected.gpr.set64(EAX, 0);
    expected.gpr.set64(ECX, count);
    expected.flags.set_cf(true);
    {
        Interpreter interpreter(expected, *m_memory);
        ASSERT_EQ(interpreter.run(static_cast<u32>(count * 3)),
                  ExecResult::Continue);
    }
    expected.materialize_lazy_flags();

    CpuState actual;
    init_cpu64(actual);
    actual.gpr.set64(EAX, 0);
    actual.gpr.set64(ECX, count);
    actual.flags.set_cf(true);
    auto* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_EQ(m_jit->execute(*compiled, actual), JitExecResult::Continue);
    actual.materialize_lazy_flags();

    EXPECT_EQ(actual.gpr.get64(EAX), expected.gpr.get64(EAX));
    EXPECT_EQ(actual.gpr.get64(ECX), expected.gpr.get64(ECX));
    EXPECT_EQ(actual.rip, expected.rip);
    EXPECT_EQ(actual.flags.cf(), expected.flags.cf());
    EXPECT_EQ(actual.flags.zf(), expected.flags.zf());
}

TEST_F(IdiomFusionTest, X64InBlockJccLoopReturnsAtExactCapWithGuestFlags) {
    const u8 code[] = {
        0x48, 0xFF, 0xC0,
        0x48, 0x83, 0xE9, 0x01,
        0x75, 0xF7,
    };
    constexpr u64 count = MAX_IN_BLOCK_LOOP_ITERS + 17ull;
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));
    auto block = build_block64(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 3u);

    const auto setup = [&](CpuState& cpu) {
        init_cpu64(cpu);
        cpu.gpr.set64(EAX, 0x100000000ull);
        cpu.gpr.set64(ECX, count);
    };
    CpuState cpu;
    setup(cpu);
    auto* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
    cpu.materialize_lazy_flags();

    EXPECT_EQ(cpu.gpr.get64(EAX), 0x100000000ull +
                                      MAX_IN_BLOCK_LOOP_ITERS);
    EXPECT_EQ(cpu.gpr.get64(ECX), 17u);
    EXPECT_EQ(cpu.jit_iters, MAX_IN_BLOCK_LOOP_ITERS);
    EXPECT_FALSE(cpu.flags.cf());
    EXPECT_FALSE(cpu.flags.zf());
    EXPECT_FALSE(cpu.flags.sf());
    EXPECT_FALSE(cpu.flags.of());
    EXPECT_TRUE(cpu.flags.pf());

    // The capped iteration may stop on either side of its JNZ: before it
    // retires (RIP is the JNZ, as Tier 0 exits) or after (RIP is the taken
    // target, as Tier 1 exits). Both are precise instruction boundaries, so
    // the state must equal the interpreter's at one of them.
    const u32 cap_insns = static_cast<u32>(MAX_IN_BLOCK_LOOP_ITERS) * 3u;
    bool matched = false;
    for (const u32 steps : {cap_insns - 1u, cap_insns}) {
        CpuState expected;
        setup(expected);
        Interpreter interpreter(expected, *m_memory);
        const ExecResult result = interpreter.run(steps);
        ASSERT_TRUE(result == ExecResult::Continue ||
                    result == ExecResult::Branch);
        expected.materialize_lazy_flags();
        if (expected.rip != cpu.rip) continue;
        matched = true;
        EXPECT_EQ(cpu.gpr.get64(EAX), expected.gpr.get64(EAX));
        EXPECT_EQ(cpu.gpr.get64(ECX), expected.gpr.get64(ECX));
        EXPECT_EQ(cpu.flags.rflags & 0x8D5u, expected.flags.rflags & 0x8D5u);
    }
    EXPECT_TRUE(matched) << "exit RIP 0x" << std::hex << cpu.rip
                         << " is neither the JNZ (0x1007) nor its target";
}

TEST_F(IdiomFusionTest, FusedMemcpyParity) {
    // 2 MOVDQU loads from [ESI+0], [ESI+16] into XMM0, XMM1
    // 2 MOVDQU stores from XMM0, XMM1 to [EDI+0], [EDI+16]
    const u8 code[] = {
        0xF3, 0x0F, 0x6F, 0x06,
        0xF3, 0x0F, 0x6F, 0x4E, 0x10,
        0xF3, 0x0F, 0x7F, 0x07,
        0xF3, 0x0F, 0x7F, 0x4F, 0x10
    };

    // Write recognizable pattern at source area (0x3000)
    u8 src_data[32];
    for (int i = 0; i < 32; ++i) src_data[i] = static_cast<u8>(i + 0xA0);
    ASSERT_TRUE(m_memory->write_bytes(0x3000,
                    reinterpret_cast<const Byte*>(src_data), 32));

    // Write code
    ASSERT_TRUE(m_memory->write_bytes(0x1000,
                    reinterpret_cast<const Byte*>(code), sizeof(code)));

    // -- Interpreter run --
    CpuState icpu;
    init_cpu(icpu);
    icpu.gpr.set32(ESI, 0x3000);
    icpu.gpr.set32(EDI, 0x4000);
    {
        // Reset source data
        ASSERT_TRUE(m_memory->write_bytes(0x3000,
                        reinterpret_cast<const Byte*>(src_data), 32));
        // Clear dest
        u8 zeros[32] = {};
        ASSERT_TRUE(m_memory->write_bytes(0x4000,
                        reinterpret_cast<const Byte*>(zeros), 32));

        BasicBlock block = build_block(code, sizeof(code));
        Interpreter interp(icpu, *m_memory);
        auto result = interp.run(block.instruction_count);
        EXPECT_EQ(result, ExecResult::Continue);
    }

    u8 interp_dst[32] = {};
    ASSERT_TRUE(m_memory->read_bytes(0x4000,
                    reinterpret_cast<Byte*>(interp_dst), 32));

    // -- JIT run (with fusion) --
    CpuState jcpu;
    init_cpu(jcpu);
    jcpu.gpr.set32(ESI, 0x3000);
    jcpu.gpr.set32(EDI, 0x4000);
    {
        // Reset source data
        ASSERT_TRUE(m_memory->write_bytes(0x3000,
                        reinterpret_cast<const Byte*>(src_data), 32));
        u8 zeros[32] = {};
        ASSERT_TRUE(m_memory->write_bytes(0x4000,
                        reinterpret_cast<const Byte*>(zeros), 32));

        ASSERT_TRUE(m_memory->write_bytes(0x1000,
                        reinterpret_cast<const Byte*>(code), sizeof(code)));
        BasicBlock block = build_block(code, sizeof(code));
        CompiledBlock* cb = m_jit->compile(block);
        ASSERT_NE(cb, nullptr);
        auto jit_result = m_jit->execute(*cb, jcpu);
        EXPECT_EQ(jit_result, JitExecResult::Continue);
    }

    u8 jit_dst[32] = {};
    ASSERT_TRUE(m_memory->read_bytes(0x4000,
                    reinterpret_cast<Byte*>(jit_dst), 32));

    // Verify memory contents match
    for (int i = 0; i < 32; ++i) {
        EXPECT_EQ(interp_dst[i], jit_dst[i])
            << "Byte " << i << " diverged: interp=0x" << std::hex
            << static_cast<int>(interp_dst[i])
            << " jit=0x" << static_cast<int>(jit_dst[i]);
    }

    u8 xmm0[16]{};
    u8 xmm1[16]{};
    jcpu.sse.xmm[0].get_bytes(xmm0);
    jcpu.sse.xmm[1].get_bytes(xmm1);
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(xmm0[i], static_cast<u8>(i + 0xA0)) << "XMM0 byte " << i;
        EXPECT_EQ(xmm1[i], static_cast<u8>(i + 16 + 0xA0)) << "XMM1 byte " << i;
    }

    // Verify correct data was copied
    for (int i = 0; i < 32; ++i) {
        EXPECT_EQ(jit_dst[i], static_cast<u8>(i + 0xA0))
            << "Byte " << i << " wrong value";
    }
}
