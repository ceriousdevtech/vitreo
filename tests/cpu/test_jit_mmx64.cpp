// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Differential tests for the native x64 MMX codegen (compile_mmx):
// MOVD / MOVQ / PMULUDQ / PSRLQ / PSLLQ. These opcodes carry ~99.9% of the
// x64 decompression interpreter-delegate fallbacks (Steam self-update).
//
// MMX aliases the x87 stack, and writing an MM register sets st[n]'s
// exponent/sign halfword to 0xFFFF (the "in use" tag). A stencil that wrote
// the mantissa but not the tag would pass a mantissa-only check yet silently
// desync x87/MMX state — so every check compares FULL FpuState (all eight
// st[] mantissas AND exponent_sign halfwords) against the interpreter, plus
// GPRs and guest memory.

#include <gtest/gtest.h>
#include <cstring>
#include <functional>
#include <vector>
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

constexpr u8 RAX = static_cast<u8>(RegIndex::RAX);
constexpr u8 RSP = static_cast<u8>(RegIndex::RSP);

constexpr GuestAddr kCode = 0x1000;
constexpr GuestAddr kData = 0x4000;   // scratch for memory-operand forms

class JitMmx64Test : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(0x1000, 0x10000,
                                         MemoryProtection::ReadWrite));
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_cache = std::make_unique<BlockCache>();
        m_jit->set_block_cache(m_cache.get());
    }

    BasicBlock build_block(const std::vector<u8>& code) {
        BasicBlock block;
        block.start_addr = kCode;
        block.is_64bit = true;
        MemoryByteReader reader(code.data(), code.size());
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            if (decode_instruction(reader, DecodeMode::Mode64, inst)
                    != DecodeError::Success) {
                break;
            }
            block.instructions.push_back(inst);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count = static_cast<u32>(block.instructions.size());
        return block;
    }

    void init_cpu(CpuState& cpu) {
        cpu.init_long_mode();
        cpu.gpr.set64(RSP, 0x10000);
        cpu.rip = kCode;
    }

    /// Run the same code through interpreter and JIT; require identical GPRs,
    /// full FPU/MMX state, guest memory, and fault outcome. Fails (not skips)
    /// if the JIT declines the opcode — the whole point is that it compiles.
    void check(const std::vector<u8>& code,
               std::function<void(CpuState&)> setup = {}) {
        ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
        BasicBlock block = build_block(code);
        ASSERT_GT(block.instruction_count, 0u) << "decode failed";

        std::vector<u8> initial(0x1000);
        ASSERT_TRUE(m_memory->read_bytes(kData, initial.data(), initial.size()));

        // ---- Interpreter (reference) ----
        CpuState icpu;
        init_cpu(icpu);
        if (setup) setup(icpu);
        ExecResult ir;
        {
            Interpreter interp(icpu, *m_memory);
            ir = interp.run(block.instruction_count);
        }
        icpu.materialize_lazy_flags();
        std::vector<u8> imem(0x1000);
        ASSERT_TRUE(m_memory->read_bytes(kData, imem.data(), imem.size()));
        const bool iraised = (ir == ExecResult::Exception) || icpu.pending_raise.active;

        ASSERT_TRUE(m_memory->write_bytes(kData, initial.data(), initial.size()));

        // ---- JIT ----
        CpuState jcpu;
        init_cpu(jcpu);
        if (setup) setup(jcpu);
        CompiledBlock* cb = m_jit->compile(block);
        ASSERT_NE(cb, nullptr) << "JIT compilation failed";
        (void)m_jit->execute(*cb, jcpu);
        jcpu.materialize_lazy_flags();
        std::vector<u8> jmem(0x1000);
        ASSERT_TRUE(m_memory->read_bytes(kData, jmem.data(), jmem.size()));
        const bool jraised = jcpu.pending_raise.active;

        EXPECT_EQ(iraised, jraised) << "fault outcome diverged";

        for (u8 i = 0; i < 16; ++i) {
            EXPECT_EQ(icpu.gpr.get64(i), jcpu.gpr.get64(i))
                << "GPR " << static_cast<unsigned>(i) << " diverged";
        }

        // Full MMX/x87 state: mantissa AND the exponent/sign tag for all 8.
        for (u8 i = 0; i < 8; ++i) {
            EXPECT_EQ(icpu.fpu.st[i].mantissa, jcpu.fpu.st[i].mantissa)
                << "MM" << static_cast<unsigned>(i) << " mantissa diverged";
            EXPECT_EQ(icpu.fpu.st[i].exponent_sign, jcpu.fpu.st[i].exponent_sign)
                << "MM" << static_cast<unsigned>(i)
                << " x87 tag (exponent_sign) diverged — set_mm side effect missed";
        }

        ASSERT_EQ(imem.size(), jmem.size());
        for (usize i = 0; i < imem.size(); ++i) {
            if (imem[i] != jmem[i]) {
                ADD_FAILURE() << "memory diverged at 0x" << std::hex
                              << (kData + i) << ": interp=0x"
                              << static_cast<unsigned>(imem[i]) << " jit=0x"
                              << static_cast<unsigned>(jmem[i]);
                break;
            }
        }

        ASSERT_TRUE(m_memory->write_bytes(kData, initial.data(), initial.size()));
    }

    // Seed an MM register (also stamps the 0xFFFF tag, matching guest state
    // after any prior MMX write).
    static std::function<void(CpuState&)> seed_mm(u8 reg, u64 val) {
        return [reg, val](CpuState& c) { c.fpu.set_mm(reg, val); };
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
    std::unique_ptr<BlockCache> m_cache;
};

// ---- MOVD mm, r/m32 (0F 6E) ----

TEST_F(JitMmx64Test, MovdMmFromGpr_ZeroExtends) {
    // MOVD MM0, EAX
    check({0x0F, 0x6E, 0xC0},
          [](CpuState& c) { c.gpr.set64(RAX, 0xFFFFFFFFDEADBEEFull); });
}

TEST_F(JitMmx64Test, MovdMmFromMem) {
    // MOVD MM3, [RAX]
    check({0x0F, 0x6E, 0x18},
          [](CpuState& c) {
              c.gpr.set64(RAX, kData);
          });
}

// ---- MOVD r/m32, mm (0F 7E) ----

TEST_F(JitMmx64Test, MovdGprFromMm_ZeroExtendsUpper) {
    // MOVD EAX, MM0 — writing EAX must zero the upper 32 of RAX.
    check({0x0F, 0x7E, 0xC0},
          [](CpuState& c) {
              c.gpr.set64(RAX, 0x1111111122222222ull);
              c.fpu.set_mm(0, 0xAAAAAAAABBBBBBBBull);
          });
}

TEST_F(JitMmx64Test, MovdMemFromMm) {
    // MOVD [RAX], MM2
    check({0x0F, 0x7E, 0x10},
          [](CpuState& c) {
              c.gpr.set64(RAX, kData);
              c.fpu.set_mm(2, 0x99887766AABBCCDDull);
          });
}

// ---- MOVQ mm, mm/m64 (0F 6F) ----

TEST_F(JitMmx64Test, MovqMmFromMm) {
    // MOVQ MM0, MM1
    check({0x0F, 0x6F, 0xC1}, seed_mm(1, 0x0123456789ABCDEFull));
}

TEST_F(JitMmx64Test, MovqMmFromMem) {
    // MOVQ MM0, [RAX]
    check({0x0F, 0x6F, 0x00},
          [](CpuState& c) { c.gpr.set64(RAX, kData); });
}

// ---- MOVQ mm/m64, mm (0F 7F) ----

TEST_F(JitMmx64Test, MovqMmFromMm_StoreForm) {
    // MOVQ MM1, MM0
    check({0x0F, 0x7F, 0xC1}, seed_mm(0, 0xCAFEBABEF00DBAADull));
}

TEST_F(JitMmx64Test, MovqMemFromMm) {
    // MOVQ [RAX], MM2
    check({0x0F, 0x7F, 0x10},
          [](CpuState& c) {
              c.gpr.set64(RAX, kData);
              c.fpu.set_mm(2, 0x1122334455667788ull);
          });
}

// ---- PMULUDQ mm, mm/m64 (0F F4) ----

TEST_F(JitMmx64Test, PmuludqReg) {
    // PMULUDQ MM0, MM1 — only low dwords participate.
    check({0x0F, 0xF4, 0xC1},
          [](CpuState& c) {
              c.fpu.set_mm(0, 0xFFFFFFFF0000FFFFull);  // low = 0x0000FFFF
              c.fpu.set_mm(1, 0xDEADBEEF00010001ull);  // low = 0x00010001
          });
}

TEST_F(JitMmx64Test, PmuludqRegMaxDwords) {
    // 0xFFFFFFFF * 0xFFFFFFFF must land in the full 64-bit product.
    check({0x0F, 0xF4, 0xC1},
          [](CpuState& c) {
              c.fpu.set_mm(0, 0x00000000FFFFFFFFull);
              c.fpu.set_mm(1, 0x00000000FFFFFFFFull);
          });
}

TEST_F(JitMmx64Test, PmuludqMem) {
    // PMULUDQ MM0, [RAX]
    check({0x0F, 0xF4, 0x00},
          [](CpuState& c) {
              c.gpr.set64(RAX, kData);
              c.fpu.set_mm(0, 0x00000000ABCDEF01ull);
          });
}

// ---- PSRLQ / PSLLQ mm, imm8 (0F 73 /2, /6) ----

TEST_F(JitMmx64Test, PsrlqImm) {
    // PSRLQ MM0, 13
    check({0x0F, 0x73, 0xD0, 0x0D}, seed_mm(0, 0xFEDCBA9876543210ull));
}

TEST_F(JitMmx64Test, PsllqImm) {
    // PSLLQ MM0, 20
    check({0x0F, 0x73, 0xF0, 0x14}, seed_mm(0, 0x00000000FEDCBA98ull));
}

TEST_F(JitMmx64Test, PsrlqImmZero) {
    // PSRLQ MM0, 0 — no value change, but still an MMX write (tag).
    check({0x0F, 0x73, 0xD0, 0x00}, seed_mm(0, 0x123456789ABCDEF0ull));
}

TEST_F(JitMmx64Test, PsrlqImmOverMax) {
    // PSRLQ MM0, 70 — count > 63 yields 0 on x86 (not a masked shift).
    check({0x0F, 0x73, 0xD0, 0x46}, seed_mm(0, 0xFFFFFFFFFFFFFFFFull));
}

TEST_F(JitMmx64Test, PsllqImmExactly64) {
    // PSLLQ MM0, 64 — boundary; must be 0, not an unchanged value.
    check({0x0F, 0x73, 0xF0, 0x40}, seed_mm(0, 0xFFFFFFFFFFFFFFFFull));
}

// ---- The decoder-loop shape these exist for: MOVD, PMULUDQ, PSRLQ chain ----

TEST_F(JitMmx64Test, MultiplyShiftChain) {
    std::vector<u8> code = {
        0x0F, 0x6E, 0xC0,        // MOVD MM0, EAX
        0x0F, 0x6E, 0xCB,        // MOVD MM1, EBX
        0x0F, 0xF4, 0xC1,        // PMULUDQ MM0, MM1
        0x0F, 0x73, 0xD0, 0x20,  // PSRLQ MM0, 32
        0x0F, 0x7E, 0xC2,        // MOVD EDX, MM0
    };
    check(code, [](CpuState& c) {
        c.gpr.set64(RAX, 0x00000000C0FFEE01ull);
        c.gpr.set64(static_cast<u8>(RegIndex::RBX), 0x000000001234ABCDull);
    });
}

// ===========================================================================
// 32-bit mode — the actual Steam extraction path. The decompressor runs in
// the PE32 bootstrapper, so these ops must JIT correctly in 32-bit mode too.
// Same differential comparison; only the block/CPU mode differs.
// ===========================================================================

class JitMmx32Test : public JitMmx64Test {
protected:
    BasicBlock build_block32(const std::vector<u8>& code) {
        BasicBlock block;
        block.start_addr = kCode;
        block.is_64bit = false;
        MemoryByteReader reader(code.data(), code.size());
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

    void check32(const std::vector<u8>& code,
                 std::function<void(CpuState&)> setup = {}) {
        ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
        BasicBlock block = build_block32(code);
        ASSERT_GT(block.instruction_count, 0u) << "decode failed";

        std::vector<u8> initial(0x1000);
        ASSERT_TRUE(m_memory->read_bytes(kData, initial.data(), initial.size()));

        CpuState icpu;
        icpu.init_protected_mode();
        icpu.gpr.set32(RSP, 0x10000);
        icpu.rip = kCode;
        if (setup) setup(icpu);
        ExecResult ir;
        {
            Interpreter interp(icpu, *m_memory);
            ir = interp.run(block.instruction_count);
        }
        icpu.materialize_lazy_flags();
        std::vector<u8> imem(0x1000);
        ASSERT_TRUE(m_memory->read_bytes(kData, imem.data(), imem.size()));
        const bool iraised = (ir == ExecResult::Exception) || icpu.pending_raise.active;

        ASSERT_TRUE(m_memory->write_bytes(kData, initial.data(), initial.size()));

        CpuState jcpu;
        jcpu.init_protected_mode();
        jcpu.gpr.set32(RSP, 0x10000);
        jcpu.rip = kCode;
        if (setup) setup(jcpu);
        CompiledBlock* cb = m_jit->compile(block);
        ASSERT_NE(cb, nullptr) << "JIT compilation failed";
        (void)m_jit->execute(*cb, jcpu);
        jcpu.materialize_lazy_flags();
        std::vector<u8> jmem(0x1000);
        ASSERT_TRUE(m_memory->read_bytes(kData, jmem.data(), jmem.size()));
        const bool jraised = jcpu.pending_raise.active;

        EXPECT_EQ(iraised, jraised) << "fault outcome diverged";
        for (u8 i = 0; i < 8; ++i) {
            EXPECT_EQ(icpu.gpr.get32(i), jcpu.gpr.get32(i))
                << "GPR " << static_cast<unsigned>(i) << " diverged";
        }
        for (u8 i = 0; i < 8; ++i) {
            EXPECT_EQ(icpu.fpu.st[i].mantissa, jcpu.fpu.st[i].mantissa)
                << "MM" << static_cast<unsigned>(i) << " mantissa diverged";
            EXPECT_EQ(icpu.fpu.st[i].exponent_sign, jcpu.fpu.st[i].exponent_sign)
                << "MM" << static_cast<unsigned>(i) << " x87 tag diverged";
        }
        ASSERT_EQ(imem.size(), jmem.size());
        for (usize i = 0; i < imem.size(); ++i) {
            if (imem[i] != jmem[i]) {
                ADD_FAILURE() << "memory diverged at 0x" << std::hex
                              << (kData + i);
                break;
            }
        }
        ASSERT_TRUE(m_memory->write_bytes(kData, initial.data(), initial.size()));
    }
};

TEST_F(JitMmx32Test, MovdMmFromGpr) {
    check32({0x0F, 0x6E, 0xC0},
            [](CpuState& c) { c.gpr.set32(RAX, 0xDEADBEEF); });
}

TEST_F(JitMmx32Test, MovdGprFromMm) {
    check32({0x0F, 0x7E, 0xC0}, seed_mm(0, 0xAABBCCDD11223344ull));
}

TEST_F(JitMmx32Test, MovqMmFromMm) {
    check32({0x0F, 0x6F, 0xC1}, seed_mm(1, 0x0123456789ABCDEFull));
}

TEST_F(JitMmx32Test, PmuludqReg) {
    check32({0x0F, 0xF4, 0xC1},
            [](CpuState& c) {
                c.fpu.set_mm(0, 0x00000000FFFFFFFFull);
                c.fpu.set_mm(1, 0x00000000FFFFFFFFull);
            });
}

TEST_F(JitMmx32Test, PsrlqImm) {
    check32({0x0F, 0x73, 0xD0, 0x0D}, seed_mm(0, 0xFEDCBA9876543210ull));
}

TEST_F(JitMmx32Test, PsllqImmOverMax) {
    check32({0x0F, 0x73, 0xF0, 0x46}, seed_mm(0, 0xFFFFFFFFFFFFFFFFull));
}

// 32-bit memory operand: [EAX] with a flat DS. Exercises emit_compute_ea +
// the 64-bit-safe memory helper path from the 32-bit dispatch.
TEST_F(JitMmx32Test, MovqMmFromMem) {
    check32({0x0F, 0x6F, 0x00},
            [](CpuState& c) { c.gpr.set32(RAX, static_cast<u32>(kData)); });
}

TEST_F(JitMmx32Test, MovqMemFromMm) {
    check32({0x0F, 0x7F, 0x10},
            [](CpuState& c) {
                c.gpr.set32(RAX, static_cast<u32>(kData));
                c.fpu.set_mm(2, 0x1122334455667788ull);
            });
}

TEST_F(JitMmx32Test, PmuludqMem) {
    check32({0x0F, 0xF4, 0x00},
            [](CpuState& c) {
                c.gpr.set32(RAX, static_cast<u32>(kData));
                c.fpu.set_mm(0, 0x00000000ABCDEF01ull);
            });
}

// The decoder-loop shape, in 32-bit mode.
TEST_F(JitMmx32Test, MultiplyShiftChain) {
    std::vector<u8> code = {
        0x0F, 0x6E, 0xC0,        // MOVD MM0, EAX
        0x0F, 0x6E, 0xCB,        // MOVD MM1, EBX
        0x0F, 0xF4, 0xC1,        // PMULUDQ MM0, MM1
        0x0F, 0x73, 0xD0, 0x20,  // PSRLQ MM0, 32
        0x0F, 0x7E, 0xC2,        // MOVD EDX, MM0
    };
    check32(code, [](CpuState& c) {
        c.gpr.set32(RAX, 0xC0FFEE01);
        c.gpr.set32(static_cast<u8>(RegIndex::RBX), 0x1234ABCD);
    });
}

}  // namespace
