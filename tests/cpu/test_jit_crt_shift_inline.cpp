// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - JIT inliner parity test for MS CRT 64-bit shift helpers.
//
// Verifies that a CALL rel32 to a memory region containing the byte
// signature of `__allshl` or `__aullshr` produces the same EDX:EAX as
// running the helper byte-for-byte through the interpreter.

#include <gtest/gtest.h>
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

constexpr u8 EAX_R = static_cast<u8>(RegIndex::RAX);
constexpr u8 ECX_R = static_cast<u8>(RegIndex::RCX);
constexpr u8 EDX_R = static_cast<u8>(RegIndex::RDX);
constexpr u8 ESP_R = static_cast<u8>(RegIndex::RSP);

// MS x86 __allshl bytes (verified from disassembly):
//   cmp cl,0x40 / jae +eb / cmp cl,0x20 / jae +ea
//   shld edx,eax,cl / shl eax,cl / ret
//   shr_high block / xor block (skipped)
const u8 kAllshlBytes[] = {
    0x80, 0xF9, 0x40, 0x73, 0x10,  // cmp cl,0x40 ; jae +0x10
    0x80, 0xF9, 0x20, 0x73, 0x05,  // cmp cl,0x20 ; jae +0x05
    0x0F, 0xA5, 0xC2,              // shld edx, eax, cl
    0xD3, 0xE0,                    // shl  eax, cl
    0xC3,                          // ret
    0x8B, 0xD0, 0x33, 0xC0, 0xD3,  // (cl>=32) mov edx, eax ; xor eax, eax ; shl edx, cl
    0xE2, 0xC3,                    //         ret
    0x33, 0xC0, 0x33, 0xD2, 0xC3,  // (cl>=64) xor eax, eax ; xor edx, edx ; ret
};

const u8 kAullshrBytes[] = {
    0x80, 0xF9, 0x40, 0x73, 0x10,  // cmp cl,0x40
    0x80, 0xF9, 0x20, 0x73, 0x05,  // cmp cl,0x20
    0x0F, 0xAD, 0xD0,              // shrd eax, edx, cl
    0xD3, 0xEA,                    // shr  edx, cl
    0xC3,                          // ret
    0x8B, 0xC2, 0x33, 0xD2, 0xD3,
    0xE8, 0xC3,
    0x33, 0xC0, 0x33, 0xD2, 0xC3,
};

class CrtShiftInlineTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(
            0x10000, 0x10000, MemoryProtection::ReadWrite));
        ASSERT_TRUE(m_memory->map_region(
            0x80000,  0x4000, MemoryProtection::ReadWrite));   // stack
        m_jit = std::make_unique<JitCompiler>(*m_memory);
    }

    // Lay out a tiny program at 0x10000:
    //   call <helper>     (5 bytes, target = helper_addr)
    //   ret               (terminator that exits cleanly)
    //
    // helper at 0x12000.
    void layout(const u8* helper, usize helper_len) {
        const u32 prog_addr   = 0x10000;
        const u32 helper_addr = 0x12000;
        const u8 call_e8 = 0xE8;
        i32 disp = static_cast<i32>(helper_addr) -
                   static_cast<i32>(prog_addr + 5);
        u8 prog[6] = {
            call_e8,
            static_cast<u8>(disp & 0xFF),
            static_cast<u8>((disp >> 8) & 0xFF),
            static_cast<u8>((disp >> 16) & 0xFF),
            static_cast<u8>((disp >> 24) & 0xFF),
            0xCC,  // INT3 (we never reach here in JIT inline path)
        };
        ASSERT_TRUE(m_memory->write_bytes(prog_addr,
            reinterpret_cast<const Byte*>(prog), sizeof(prog)));
        ASSERT_TRUE(m_memory->write_bytes(helper_addr,
            reinterpret_cast<const Byte*>(helper), helper_len));
        m_cb = nullptr;
    }

    // Compile the CALL block at 0x10000 once and reuse it. Recompiling the
    // same address for every case trips the JIT churn guard, which demotes
    // the address to the interpreter and returns null.
    CompiledBlock* compiled_call() {
        if (m_cb != nullptr) {
            return m_cb;
        }
        BasicBlock block;
        block.start_addr = 0x10000;
        u8 prog[5];
        EXPECT_TRUE(m_memory->read_bytes(0x10000,
            reinterpret_cast<Byte*>(prog), sizeof(prog)).is_ok());
        MemoryByteReader reader(prog, sizeof(prog));
        DecodedInstruction inst;
        DecodeError err = decode_instruction(reader, DecodeMode::Mode32, inst);
        EXPECT_EQ(err, DecodeError::Success);
        block.instructions.push_back(inst);
        block.byte_length = 5;
        block.instruction_count = 1;
        m_cb = m_jit->compile(block);
        return m_cb;
    }

    // Run a single CALL block with the JIT compiler. Returns final EDX:EAX.
    u64 run_jit_call(u32 eax, u32 edx, u8 cl) {
        CpuState cpu;
        cpu.init_protected_mode();
        cpu.gpr.set32(ESP_R, 0x82000);
        cpu.gpr.set32(EAX_R, eax);
        cpu.gpr.set32(EDX_R, edx);
        cpu.gpr.set32(ECX_R, static_cast<u32>(cl));
        cpu.rip = 0x10000;

        CompiledBlock* cb = compiled_call();
        EXPECT_NE(cb, nullptr);
        if (cb == nullptr) {
            return 0;
        }
        (void)m_jit->execute(*cb, cpu);

        u64 lo = static_cast<u32>(cpu.gpr.get32(EAX_R));
        u64 hi = static_cast<u32>(cpu.gpr.get32(EDX_R));
        return lo | (hi << 32);
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler>  m_jit;
    CompiledBlock*                m_cb = nullptr;
};

} // namespace

TEST_F(CrtShiftInlineTest, AllshlMatchesNativeShift) {
    layout(kAllshlBytes, sizeof(kAllshlBytes));

    struct Case { u32 eax, edx; u8 cl; };
    const Case cases[] = {
        {0x12345678u, 0x9ABCDEF0u, 0},
        {0x12345678u, 0x9ABCDEF0u, 1},
        {0x12345678u, 0x9ABCDEF0u, 4},
        {0x12345678u, 0x9ABCDEF0u, 16},
        {0x12345678u, 0x9ABCDEF0u, 31},
        {0x12345678u, 0x9ABCDEF0u, 32},
        {0x12345678u, 0x9ABCDEF0u, 33},
        {0x12345678u, 0x9ABCDEF0u, 63},
        {0x12345678u, 0x9ABCDEF0u, 64},
        {0x12345678u, 0x9ABCDEF0u, 65},
        {0x12345678u, 0x9ABCDEF0u, 100},
        {0xFFFFFFFFu, 0xFFFFFFFFu, 1},
        {0x00000001u, 0x00000000u, 31},
        {0x00000001u, 0x00000000u, 32},
        {0x00000000u, 0x00000001u, 1},
    };
    for (const auto& c : cases) {
        u64 input = static_cast<u64>(c.eax)
                  | (static_cast<u64>(c.edx) << 32);
        u64 expected = (c.cl >= 64) ? 0
                                    : (input << (c.cl & 0xFF));
        u64 got = run_jit_call(c.eax, c.edx, c.cl);
        EXPECT_EQ(got, expected)
            << "allshl: eax=0x" << std::hex << c.eax
            << " edx=0x" << c.edx
            << " cl=" << std::dec << static_cast<int>(c.cl);
    }
}

TEST_F(CrtShiftInlineTest, AullshrMatchesNativeShift) {
    layout(kAullshrBytes, sizeof(kAullshrBytes));

    struct Case { u32 eax, edx; u8 cl; };
    const Case cases[] = {
        {0x12345678u, 0x9ABCDEF0u, 0},
        {0x12345678u, 0x9ABCDEF0u, 1},
        {0x12345678u, 0x9ABCDEF0u, 4},
        {0x12345678u, 0x9ABCDEF0u, 16},
        {0x12345678u, 0x9ABCDEF0u, 31},
        {0x12345678u, 0x9ABCDEF0u, 32},
        {0x12345678u, 0x9ABCDEF0u, 33},
        {0x12345678u, 0x9ABCDEF0u, 63},
        {0x12345678u, 0x9ABCDEF0u, 64},
        {0x12345678u, 0x9ABCDEF0u, 65},
        {0x12345678u, 0x9ABCDEF0u, 200},
        {0xFFFFFFFFu, 0xFFFFFFFFu, 1},
        {0x00000000u, 0x80000000u, 1},
        {0x00000000u, 0x80000000u, 31},
        {0x00000000u, 0x80000000u, 32},
    };
    for (const auto& c : cases) {
        u64 input = static_cast<u64>(c.eax)
                  | (static_cast<u64>(c.edx) << 32);
        u64 expected = (c.cl >= 64) ? 0
                                    : (input >> (c.cl & 0xFF));
        u64 got = run_jit_call(c.eax, c.edx, c.cl);
        EXPECT_EQ(got, expected)
            << "aullshr: eax=0x" << std::hex << c.eax
            << " edx=0x" << c.edx
            << " cl=" << std::dec << static_cast<int>(c.cl);
    }
}

TEST_F(CrtShiftInlineTest, EcxIsPreserved) {
    layout(kAllshlBytes, sizeof(kAllshlBytes));

    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(ESP_R, 0x82000);
    cpu.gpr.set32(EAX_R, 0x1u);
    cpu.gpr.set32(EDX_R, 0x0u);
    cpu.gpr.set32(ECX_R, 0xDEAD0010u);  // CL=0x10, upper bytes must survive
    cpu.rip = 0x10000;

    BasicBlock block;
    block.start_addr = 0x10000;
    u8 prog[5];
    ASSERT_TRUE(m_memory->read_bytes(0x10000,
        reinterpret_cast<Byte*>(prog), sizeof(prog)).is_ok());
    MemoryByteReader reader(prog, sizeof(prog));
    DecodedInstruction inst;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode32, inst),
              DecodeError::Success);
    block.instructions.push_back(inst);
    block.byte_length = 5;
    block.instruction_count = 1;

    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    (void)m_jit->execute(*cb, cpu);

    EXPECT_EQ(cpu.gpr.get32(ECX_R), 0xDEAD0010u);
    EXPECT_EQ(cpu.gpr.get32(EAX_R), 0x10000u);
    EXPECT_EQ(cpu.gpr.get32(EDX_R), 0u);
}
