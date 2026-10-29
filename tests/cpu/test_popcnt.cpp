// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests for POPCNT instruction

#include <gtest/gtest.h>
#include <cstring>
#include <functional>
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

constexpr u8 EAX = static_cast<u8>(RegIndex::RAX);
constexpr u8 ECX = static_cast<u8>(RegIndex::RCX);
constexpr u8 EDX = static_cast<u8>(RegIndex::RDX);
constexpr u8 EBX = static_cast<u8>(RegIndex::RBX);
constexpr u8 ESP = static_cast<u8>(RegIndex::RSP);

class PopcntTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        auto map_result = m_memory->map_region(0x1000, 0x10000,
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

    void init_cpu(CpuState& cpu) {
        cpu.init_protected_mode();
        cpu.gpr.set32(ESP, 0x10000);
        cpu.rip = 0x1000;
    }

    void check_parity(const u8* code, usize len,
                      std::function<void(CpuState&)> setup = {}) {
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code, len));

        BasicBlock block = build_block(code, len);
        ASSERT_GT(block.instruction_count, 0u) << "No instructions decoded";

        CpuState icpu;
        init_cpu(icpu);
        if (setup) setup(icpu);
        {
            Interpreter interp(icpu, *m_memory);
            auto result = interp.run(block.instruction_count);
            EXPECT_EQ(result, ExecResult::Continue) << "Interpreter failed";
        }
        icpu.materialize_lazy_flags();

        CpuState jcpu;
        init_cpu(jcpu);
        if (setup) setup(jcpu);
        CompiledBlock* cb = m_jit->compile(block);
        ASSERT_NE(cb, nullptr) << "JIT compilation failed";
        auto jit_result = m_jit->execute(*cb, jcpu);
        EXPECT_EQ(jit_result, JitExecResult::Continue)
            << "JIT execution failed";
        jcpu.materialize_lazy_flags();

        for (int i = 0; i < 8; i++) {
            EXPECT_EQ(icpu.gpr.get32(i), jcpu.gpr.get32(i))
                << "GPR " << i << " diverged: interp=0x" << std::hex
                << icpu.gpr.get32(i) << " jit=0x" << jcpu.gpr.get32(i);
        }
        EXPECT_EQ(static_cast<u32>(icpu.rip), static_cast<u32>(jcpu.rip))
            << "EIP diverged";
        EXPECT_EQ(icpu.flags.cf(), jcpu.flags.cf()) << "CF diverged";
        EXPECT_EQ(icpu.flags.zf(), jcpu.flags.zf()) << "ZF diverged";
        EXPECT_EQ(icpu.flags.sf(), jcpu.flags.sf()) << "SF diverged";
        EXPECT_EQ(icpu.flags.of(), jcpu.flags.of()) << "OF diverged";
        EXPECT_EQ(icpu.flags.pf(), jcpu.flags.pf()) << "PF diverged";
    }

    void check_parity(std::initializer_list<u8> code,
                      std::function<void(CpuState&)> setup = {}) {
        std::vector<u8> v(code);
        check_parity(v.data(), v.size(), setup);
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
};

// ============================================================================
// POPCNT r32, r32 — F3 0F B8 /r
// ============================================================================

// POPCNT EAX, ECX — count bits in 0x00000000 = 0, ZF should be set
TEST_F(PopcntTest, PopcntZero) {
    // F3 0F B8 C1 = POPCNT EAX, ECX
    check_parity({0xF3, 0x0F, 0xB8, 0xC1}, [](CpuState& cpu) {
        cpu.gpr.set32(ECX, 0x00000000);
    });
}

// POPCNT EAX, ECX — count bits in 0xFFFFFFFF = 32
TEST_F(PopcntTest, PopcntAllOnes) {
    check_parity({0xF3, 0x0F, 0xB8, 0xC1}, [](CpuState& cpu) {
        cpu.gpr.set32(ECX, 0xFFFFFFFF);
    });
}

// POPCNT EAX, ECX — count bits in 0x80000001 = 2
TEST_F(PopcntTest, PopcntSparse) {
    check_parity({0xF3, 0x0F, 0xB8, 0xC1}, [](CpuState& cpu) {
        cpu.gpr.set32(ECX, 0x80000001);
    });
}

// POPCNT EAX, ECX — count bits in 0x55555555 = 16
TEST_F(PopcntTest, PopcntAlternating) {
    check_parity({0xF3, 0x0F, 0xB8, 0xC1}, [](CpuState& cpu) {
        cpu.gpr.set32(ECX, 0x55555555);
    });
}

// POPCNT EAX, ECX — count bits in 0x00000001 = 1
TEST_F(PopcntTest, PopcntSingleBit) {
    check_parity({0xF3, 0x0F, 0xB8, 0xC1}, [](CpuState& cpu) {
        cpu.gpr.set32(ECX, 0x00000001);
    });
}

// POPCNT EDX, EBX — different register pair
TEST_F(PopcntTest, PopcntDiffRegs) {
    // F3 0F B8 D3 = POPCNT EDX, EBX
    check_parity({0xF3, 0x0F, 0xB8, 0xD3}, [](CpuState& cpu) {
        cpu.gpr.set32(EBX, 0x0F0F0F0F);
    });
}

// POPCNT EAX, [mem] — memory operand
TEST_F(PopcntTest, PopcntFromMemory) {
    // F3 0F B8 05 00 20 00 00 = POPCNT EAX, DWORD PTR [0x2000]
    u32 val = 0xDEADBEEF;
    ASSERT_TRUE(m_memory->write_bytes(0x2000, reinterpret_cast<const Byte*>(&val), 4));
    check_parity({0xF3, 0x0F, 0xB8, 0x05, 0x00, 0x20, 0x00, 0x00});
}

// POPCNT same register (EAX, EAX) — in-place
TEST_F(PopcntTest, PopcntSameReg) {
    // F3 0F B8 C0 = POPCNT EAX, EAX
    check_parity({0xF3, 0x0F, 0xB8, 0xC0}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0x12345678);
    });
}
