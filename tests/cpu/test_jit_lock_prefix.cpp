// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests for LOCK-prefixed instructions

#include <gtest/gtest.h>
#include <cstring>
#include <functional>
#include <vector>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_helpers.hpp"
#include "core/memory/write_watch_traps.hpp"
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
constexpr u8 EDI = static_cast<u8>(RegIndex::RDI);

class JitLockPrefixTest : public ::testing::Test {
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
            DecodeError err = decode_instruction(reader, DecodeMode::Mode32, inst);
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
                      std::function<void(CpuState&)> setup = {},
                      bool check_flags = true) {
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code, len));

        std::vector<u8> initial_mem(0x10000);
        ASSERT_TRUE(m_memory->read_bytes(0x1000, initial_mem.data(),
                                          initial_mem.size()));

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

        std::vector<u8> interp_mem(0x10000);
        ASSERT_TRUE(m_memory->read_bytes(0x1000, interp_mem.data(),
                                          interp_mem.size()));

        ASSERT_TRUE(m_memory->write_bytes(0x1000, initial_mem.data(),
                                           initial_mem.size()));

        CpuState jcpu;
        init_cpu(jcpu);
        if (setup) setup(jcpu);
        CompiledBlock* cb = m_jit->compile(block);
        ASSERT_NE(cb, nullptr) << "JIT compilation failed";
        auto jit_result = m_jit->execute(*cb, jcpu);
        EXPECT_EQ(jit_result, JitExecResult::Continue)
            << "JIT execution failed";
        jcpu.materialize_lazy_flags();

        std::vector<u8> jit_mem(0x10000);
        ASSERT_TRUE(m_memory->read_bytes(0x1000, jit_mem.data(),
                                          jit_mem.size()));

        for (int i = 0; i < 8; i++) {
            EXPECT_EQ(icpu.gpr.get32(i), jcpu.gpr.get32(i))
                << "GPR " << i << " diverged: interp=0x" << std::hex
                << icpu.gpr.get32(i) << " jit=0x" << jcpu.gpr.get32(i);
        }
        EXPECT_EQ(static_cast<u32>(icpu.rip), static_cast<u32>(jcpu.rip))
            << "EIP diverged";
        if (check_flags) {
            EXPECT_EQ(icpu.flags.cf(), jcpu.flags.cf()) << "CF diverged";
            EXPECT_EQ(icpu.flags.zf(), jcpu.flags.zf()) << "ZF diverged";
            EXPECT_EQ(icpu.flags.sf(), jcpu.flags.sf()) << "SF diverged";
            EXPECT_EQ(icpu.flags.of(), jcpu.flags.of()) << "OF diverged";
            EXPECT_EQ(icpu.flags.pf(), jcpu.flags.pf()) << "PF diverged";
        }
        for (usize i = 0; i < interp_mem.size(); ++i) {
            EXPECT_EQ(interp_mem[i], jit_mem[i])
                << "Memory diverged at 0x" << std::hex
                << (0x1000u + static_cast<u32>(i));
        }
    }

    void check_parity(std::initializer_list<u8> code,
                      std::function<void(CpuState&)> setup = {},
                      bool check_flags = true) {
        std::vector<u8> v(code);
        check_parity(v.data(), v.size(), setup, check_flags);
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
};

// ============================================================================
// Implicitly locked XCHG [m32], r32
// ============================================================================

TEST_F(JitLockPrefixTest, XchgMem32UsesImplicitLockSemantics) {
    check_parity(
        {0x87, 0x0D, 0x00, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            ASSERT_TRUE(m_memory->write_u32(0x2000, 0x11223344).is_ok());
            cpu.gpr.set32(ECX, 0x55667788);
        });
    EXPECT_EQ(m_memory->read_u32(0x2000).value_or(0), 0x55667788u);
}

// ============================================================================
// LOCK INC [m32]
// ============================================================================

TEST_F(JitLockPrefixTest, LockIncMem32) {
    // Store 41 at [0x2000], then LOCK INC DWORD PTR [0x2000]
    // F0 FF 05 00200000 = LOCK INC [0x2000]
    check_parity(
        {0xF0, 0xFF, 0x05, 0x00, 0x20, 0x00, 0x00},
        [this](CpuState&) {
            u32 val = 41;
            m_memory->write_u32(0x2000, val);
        });
    auto r = m_memory->read_u32(0x2000);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 42u);
}

TEST_F(JitLockPrefixTest, LockIncMem32Overflow) {
    // LOCK INC of 0xFFFFFFFF should wrap to 0
    check_parity(
        {0xF0, 0xFF, 0x05, 0x00, 0x20, 0x00, 0x00},
        [this](CpuState&) {
            m_memory->write_u32(0x2000, 0xFFFFFFFF);
        });
}

TEST(JitLockPrefixCrossProcessTest, LockIncIsAtomicOnSharedMapping) {
    constexpr GuestAddr address = 0x2000;
    constexpr usize mapping_size = 0x4000;
    constexpr u32 iterations = 100000;

    void* shared = ::mmap(
        nullptr, mapping_size, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(shared, MAP_FAILED);

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region_backed(
        address, mapping_size, shared, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u32(address, 0).is_ok());

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        CpuState cpu;
        cpu.init_protected_mode();
        for (u32 i = 0; i < iterations; ++i) {
            jit_lock_inc32(&cpu, &memory, address);
        }
        ::_exit(0);
    }

    CpuState cpu;
    cpu.init_protected_mode();
    for (u32 i = 0; i < iterations; ++i) {
        jit_lock_inc32(&cpu, &memory, address);
    }

    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    EXPECT_EQ(memory.read_u32(address).value_or(0), iterations * 2);
    EXPECT_EQ(::munmap(shared, mapping_size), 0);
}

// ============================================================================
// LOCK DEC [m32]
// ============================================================================

TEST_F(JitLockPrefixTest, LockDecMem32) {
    // F0 FF 0D 00200000 = LOCK DEC [0x2000]
    check_parity(
        {0xF0, 0xFF, 0x0D, 0x00, 0x20, 0x00, 0x00},
        [this](CpuState&) {
            m_memory->write_u32(0x2000, 10);
        });
    auto r = m_memory->read_u32(0x2000);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 9u);
}

TEST_F(JitLockPrefixTest, LockDecMem32ToZero) {
    // LOCK DEC of 1 should produce 0 with ZF set
    check_parity(
        {0xF0, 0xFF, 0x0D, 0x00, 0x20, 0x00, 0x00},
        [this](CpuState&) {
            m_memory->write_u32(0x2000, 1);
        });
}

// ============================================================================
// LOCK XADD [m32], r32
// ============================================================================

TEST_F(JitLockPrefixTest, LockXaddMem32) {
    // F0 0F C1 0D 00200000 = LOCK XADD [0x2000], ECX
    check_parity(
        {0xF0, 0x0F, 0xC1, 0x0D, 0x00, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            m_memory->write_u32(0x2000, 100);
            cpu.gpr.set32(ECX, 20);
        });
    auto r = m_memory->read_u32(0x2000);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 120u);
}

// ============================================================================
// LOCK CMPXCHG [m32], r32
// ============================================================================

TEST_F(JitLockPrefixTest, LockCmpxchgMatch) {
    // F0 0F B1 0D 00200000 = LOCK CMPXCHG [0x2000], ECX
    // EAX == [0x2000] → [0x2000] = ECX, ZF=1
    check_parity(
        {0xF0, 0x0F, 0xB1, 0x0D, 0x00, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            m_memory->write_u32(0x2000, 0xDEAD);
            cpu.gpr.set32(EAX, 0xDEAD);
            cpu.gpr.set32(ECX, 0xBEEF);
        });
    auto r = m_memory->read_u32(0x2000);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 0xBEEFu);
}

TEST_F(JitLockPrefixTest, LockCmpxchgMatchViaEdi) {
    // Qt QBasicMutex::lock: LOCK CMPXCHG [EDI], EDX with EAX=0, EDX=1.
    // F0 0F B1 17 = LOCK CMPXCHG [EDI], EDX
    check_parity(
        {0xF0, 0x0F, 0xB1, 0x17},
        [this](CpuState& cpu) {
            m_memory->write_u32(0x2000, 0);
            cpu.gpr.set32(EDI, 0x2000);
            cpu.gpr.set32(EAX, 0);
            cpu.gpr.set32(EDX, 1);
        });
    auto r = m_memory->read_u32(0x2000);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 1u);
}

TEST_F(JitLockPrefixTest, LockCmpxchgThenMovEdxUnlockedMutex) {
    // Qt5 QMutex::unlock: xor edx,edx; mov eax,1; lock cmpxchg [ecx],edx; mov edx,eax
    // Intel SDM Vol. 2 CMPXCHG: on mismatch EAX becomes the memory value so the
    // following MOV sees 0, not a stale pinned EAX from the MOV EAX,1.
    check_parity(
        {0x33, 0xD2, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xF0, 0x0F, 0xB1, 0x11,
         0x8B, 0xD0},
        [this](CpuState& cpu) {
            m_memory->write_u32(0x2000, 0);
            cpu.gpr.set32(ECX, 0x2000);
            cpu.gpr.set32(EAX, 0xDEAD);
            cpu.gpr.set32(EDX, 0xBEEF);
        });
    auto r = m_memory->read_u32(0x2000);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 0u);
}

TEST_F(JitLockPrefixTest, LockCmpxchgMismatch) {
    // EAX != [0x2000] → EAX = [0x2000], ZF=0
    check_parity(
        {0xF0, 0x0F, 0xB1, 0x0D, 0x00, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            m_memory->write_u32(0x2000, 0x1234);
            cpu.gpr.set32(EAX, 0x5678);
            cpu.gpr.set32(ECX, 0xBEEF);
        });
}

TEST_F(JitLockPrefixTest, LockCmpxchgRm8Match) {
    // F0 0F B0 0D 00200000 = LOCK CMPXCHG [0x2000], CL
    // AL == [0x2000] → [0x2000] = CL, ZF=1
    check_parity(
        {0xF0, 0x0F, 0xB0, 0x0D, 0x00, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            m_memory->write_u8(0x2000, 0x11);
            cpu.gpr.set8l(EAX, 0x11);
            cpu.gpr.set8l(ECX, 0x22);
        });
    auto r = m_memory->read_u8(0x2000);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 0x22u);
}

TEST_F(JitLockPrefixTest, LockCmpxchgRm8Mismatch) {
    check_parity(
        {0xF0, 0x0F, 0xB0, 0x0D, 0x00, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            m_memory->write_u8(0x2000, 0x33);
            cpu.gpr.set8l(EAX, 0x11);
            cpu.gpr.set8l(ECX, 0x22);
        });
    auto r = m_memory->read_u8(0x2000);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 0x33u);
}

// ============================================================================
// LOCK CMPXCHG8B [m64]
// ============================================================================

TEST_F(JitLockPrefixTest, LockCmpxchg8bMatch) {
    // F0 0F C7 0D 00200000 = LOCK CMPXCHG8B [0x2000]
    // EDX:EAX == [0x2000] → [0x2000] = ECX:EBX, ZF=1
    check_parity(
        {0xF0, 0x0F, 0xC7, 0x0D, 0x00, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            m_memory->write_u32(0x2000, 0xAAAAAAAA);
            m_memory->write_u32(0x2004, 0xBBBBBBBB);
            cpu.gpr.set32(EAX, 0xAAAAAAAA);
            cpu.gpr.set32(EDX, 0xBBBBBBBB);
            cpu.gpr.set32(EBX, 0x11111111);
            cpu.gpr.set32(ECX, 0x22222222);
        });
    auto lo = m_memory->read_u32(0x2000);
    auto hi = m_memory->read_u32(0x2004);
    ASSERT_TRUE(lo.is_ok() && hi.is_ok());
    EXPECT_EQ(lo.value(), 0x11111111u);
    EXPECT_EQ(hi.value(), 0x22222222u);
}

TEST_F(JitLockPrefixTest, LockCmpxchg8bMismatch) {
    // EDX:EAX != [0x2000] → EAX=[0x2000].lo, EDX=[0x2000].hi, ZF=0
    check_parity(
        {0xF0, 0x0F, 0xC7, 0x0D, 0x00, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            m_memory->write_u32(0x2000, 0x11111111);
            m_memory->write_u32(0x2004, 0x22222222);
            cpu.gpr.set32(EAX, 0xFFFFFFFF);
            cpu.gpr.set32(EDX, 0xFFFFFFFF);
            cpu.gpr.set32(EBX, 0xDEADBEEF);
            cpu.gpr.set32(ECX, 0xCAFEBABE);
        });
}

// ============================================================================
// LOCK OR / LOCK AND [m32] — Chromium AtomicFlagSet / WorkQueueSets
// ============================================================================

TEST_F(JitLockPrefixTest, LockOrMem32Imm8) {
    // F0 83 0D 00200000 01 = LOCK OR DWORD PTR [0x2000], 1
    check_parity(
        {0xF0, 0x83, 0x0D, 0x00, 0x20, 0x00, 0x00, 0x01},
        [this](CpuState&) {
            m_memory->write_u32(0x2000, 0x100);
        });
    EXPECT_EQ(m_memory->read_u32(0x2000).value_or(0), 0x101u);
}

TEST_F(JitLockPrefixTest, LockOrMem32Reg) {
    // F0 09 0D 00200000 = LOCK OR DWORD PTR [0x2000], ECX
    check_parity(
        {0xF0, 0x09, 0x0D, 0x00, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            m_memory->write_u32(0x2000, 0x00F0);
            cpu.gpr.set32(ECX, 0x0F00);
        });
    EXPECT_EQ(m_memory->read_u32(0x2000).value_or(0), 0x0FF0u);
}

TEST_F(JitLockPrefixTest, LockAndMem32Reg) {
    // F0 21 0D 00200000 = LOCK AND DWORD PTR [0x2000], ECX
    check_parity(
        {0xF0, 0x21, 0x0D, 0x00, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            m_memory->write_u32(0x2000, 0xFFFF);
            cpu.gpr.set32(ECX, 0x0F0F);
        });
    EXPECT_EQ(m_memory->read_u32(0x2000).value_or(0), 0x0F0Fu);
}

TEST_F(JitLockPrefixTest, LockAndMem16Imm8) {
    // 66 F0 83 25 00200000 FE = LOCK AND WORD PTR [0x2000], 0xFFFE
    // (imm8 sign-extended; Chromium 16-bit atomic flag clear)
    check_parity(
        {0x66, 0xF0, 0x83, 0x25, 0x00, 0x20, 0x00, 0x00, 0xFE},
        [this](CpuState&) {
            m_memory->write_u16(0x2000, 0x0003);
        });
    EXPECT_EQ(m_memory->read_u16(0x2000).value_or(0), 0x0002u);
}

TEST_F(JitLockPrefixTest, LockOrMem16Imm8) {
    // 66 F0 83 0D 00200000 01 = LOCK OR WORD PTR [0x2000], 1
    check_parity(
        {0x66, 0xF0, 0x83, 0x0D, 0x00, 0x20, 0x00, 0x00, 0x01},
        [this](CpuState&) {
            m_memory->write_u16(0x2000, 0x0002);
        });
    EXPECT_EQ(m_memory->read_u16(0x2000).value_or(0), 0x0003u);
}

TEST_F(JitLockPrefixTest, LockAddMem32Reg) {
    // F0 01 0D 00200000 = LOCK ADD DWORD PTR [0x2000], ECX
    check_parity(
        {0xF0, 0x01, 0x0D, 0x00, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            m_memory->write_u32(0x2000, 40);
            cpu.gpr.set32(ECX, 2);
        });
    EXPECT_EQ(m_memory->read_u32(0x2000).value_or(0), 42u);
}

TEST_F(JitLockPrefixTest, LockSubMem32Reg) {
    // F0 29 0D 00200000 = LOCK SUB DWORD PTR [0x2000], ECX
    check_parity(
        {0xF0, 0x29, 0x0D, 0x00, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            m_memory->write_u32(0x2000, 50);
            cpu.gpr.set32(ECX, 8);
        });
    EXPECT_EQ(m_memory->read_u32(0x2000).value_or(0), 42u);
}

TEST_F(JitLockPrefixTest, LockAdcMem32Reg) {
    // F0 11 0D 00200000 = LOCK ADC DWORD PTR [0x2000], ECX
    check_parity(
        {0xF0, 0x11, 0x0D, 0x00, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            m_memory->write_u32(0x2000, 40);
            cpu.gpr.set32(ECX, 2);
            cpu.flags.set_cf(true);
        });
    EXPECT_EQ(m_memory->read_u32(0x2000).value_or(0), 43u);
}

TEST_F(JitLockPrefixTest, LockSbbMem32Imm8) {
    // F0 83 1D 00200000 05 = LOCK SBB DWORD PTR [0x2000], 5
    check_parity(
        {0xF0, 0x83, 0x1D, 0x00, 0x20, 0x00, 0x00, 0x05},
        [this](CpuState& cpu) {
            m_memory->write_u32(0x2000, 50);
            cpu.flags.set_cf(true);
        });
    EXPECT_EQ(m_memory->read_u32(0x2000).value_or(0), 44u);
}

TEST(JitLockPrefixCrossProcessTest, LockOrIsAtomicOnSharedMapping) {
    constexpr GuestAddr address = 0x2000;
    constexpr usize mapping_size = 0x4000;
    constexpr u32 iterations = 100000;

    void* shared = ::mmap(
        nullptr, mapping_size, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(shared, MAP_FAILED);

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region_backed(
        address, mapping_size, shared, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u32(address, 0).is_ok());

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        CpuState cpu;
        cpu.init_protected_mode();
        for (u32 i = 0; i < iterations; ++i) {
            jit_lock_or32(&cpu, &memory, address, 1u << (i & 31));
        }
        ::_exit(0);
    }

    CpuState cpu;
    cpu.init_protected_mode();
    for (u32 i = 0; i < iterations; ++i) {
        jit_lock_or32(&cpu, &memory, address, 1u << (i & 31));
    }

    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    EXPECT_EQ(memory.read_u32(address).value_or(0), 0xFFFFFFFFu);
    EXPECT_EQ(::munmap(shared, mapping_size), 0);
}

// Intel SDM Vol. 2 LOCK: unmapped dest is #PF, not a completed RMW. A silent
// helper return leaves EDX/EAX stale so a later instruction in the same block
// runs as if the XADD succeeded (Qt QMutex unlock at [d_ptr+0xc] with d_ptr=0).
TEST_F(JitLockPrefixTest, LockXaddUnmappedRaisesAccessViolationAndStopsBlock) {
    // F0 0F C1 0D 0C000000 = LOCK XADD [0xC], ECX
    // B8 78563412          = MOV EAX, 0x12345678
    const u8 code[] = {
        0xF0, 0x0F, 0xC1, 0x0D, 0x0C, 0x00, 0x00, 0x00,
        0xB8, 0x78, 0x56, 0x34, 0x12,
    };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)));

    CpuState icpu;
    init_cpu(icpu);
    icpu.gpr.set32(ECX, 0xFFF00000u);
    {
        Interpreter interp(icpu, *m_memory);
        EXPECT_EQ(interp.run(2), ExecResult::Exception);
    }
    EXPECT_NE(icpu.gpr.get32(EAX), 0x12345678u);

    CpuState jcpu;
    init_cpu(jcpu);
    jcpu.gpr.set32(ECX, 0xFFF00000u);
    BasicBlock block = build_block(code, sizeof(code));
    ASSERT_GE(block.instruction_count, 2u);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    (void)m_jit->execute(*cb, jcpu);
    EXPECT_TRUE(jcpu.pending_raise.active);
    EXPECT_EQ(jcpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(jcpu.pending_raise.num_args, 2u);
    EXPECT_EQ(jcpu.pending_raise.args[1], 0xCu);
    EXPECT_NE(jcpu.gpr.get32(EAX), 0x12345678u)
        << "LOCK XADD #PF must not fall through to the next instruction";
}

// ============================================================================
// LOCK r/m64 helpers — cross-process atomicity (Intel SDM Vol. 2 LOCK)
// ============================================================================

TEST(JitLockPrefixCrossProcessTest, LockInc64IsAtomicOnSharedMapping) {
    constexpr GuestAddr address = 0x2000;
    constexpr usize mapping_size = 0x4000;
    constexpr u32 iterations = 100000;

    void* shared = ::mmap(
        nullptr, mapping_size, PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(shared, MAP_FAILED);

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region_backed(
        address, mapping_size, shared, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u64(address, 0).is_ok());

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        CpuState cpu;
        cpu.init_long_mode();
        for (u32 i = 0; i < iterations; ++i) {
            jit_lock_inc64(&cpu, &memory, address);
        }
        ::_exit(0);
    }

    CpuState cpu;
    cpu.init_long_mode();
    for (u32 i = 0; i < iterations; ++i) {
        jit_lock_inc64(&cpu, &memory, address);
    }

    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    EXPECT_EQ(memory.read_u64(address).value_or(0),
              static_cast<u64>(iterations) * 2);
    EXPECT_EQ(::munmap(shared, mapping_size), 0);
}

// ============================================================================
// Unaligned (split-lock) LOCK RMW
//
// Intel SDM Vol. 3A 9.1.2.2: the LOCK prefix works for any operand alignment.
// An unaligned operand that crosses a cache line simply escalates from a cache
// lock to a bus lock -- it is NOT a fault, and the operation is still atomic.
// Vitreo has no host atomic for a split operand, so every LOCK helper falls
// back to a plain read/modify/write serialized on the process-wide LOCK mutex.
// That whole fallback half of jit_helpers_lock.cpp had no test: every existing
// case above uses a naturally aligned address, so only the std::atomic_ref
// fast paths were ever exercised.
//
// 0x2001 is odd, so it is unaligned for the 2-, 4- and 8-byte widths at once.
// ============================================================================

namespace {
constexpr GuestAddr kUnaligned = 0x2001;

/// Registers a MEM_WRITE_WATCH region over the fixture's mapping so the
/// record_store() call each LOCK helper makes before taking the mutex becomes
/// observable, and unregisters it again on scope exit.
class WriteWatchScope {
public:
    WriteWatchScope(u64 base, u64 size)
        : m_base(base),
          m_pages(size / write_watch::kGuestPage),
          m_dirty((m_pages + 7) / 8, 0),
          m_armed((m_pages + 7) / 8, 0) {
        write_watch::register_region(base, size, m_dirty.data(),
                                     m_armed.data(), /*executable=*/false);
    }
    ~WriteWatchScope() { write_watch::unregister_region(m_base); }
    WriteWatchScope(const WriteWatchScope&) = delete;
    WriteWatchScope& operator=(const WriteWatchScope&) = delete;

    [[nodiscard]] bool page_dirty(u64 addr) const {
        return write_watch::test_bit(
            m_dirty.data(), (addr - m_base) / write_watch::kGuestPage);
    }

private:
    u64 m_base;
    u64 m_pages;
    std::vector<u8> m_dirty;
    std::vector<u8> m_armed;
};
}  // namespace

// ---------------------------------------------------------------------------
// 2- and 4-byte forms, through the real 32-bit lowering (JIT vs interpreter)
// ---------------------------------------------------------------------------

TEST_F(JitLockPrefixTest, LockAddMem32RegUnaligned) {
    // F0 01 0D 01200000 = LOCK ADD DWORD PTR [0x2001], ECX
    check_parity(
        {0xF0, 0x01, 0x0D, 0x01, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            ASSERT_TRUE(m_memory->write_u32(kUnaligned, 0x7FFFFFF0u).is_ok());
            cpu.gpr.set32(ECX, 0x20u);
        });
    // 0x7FFFFFF0 + 0x20 wraps past INT32_MAX: OF set, CF clear. check_parity
    // already compared both, so assert the value the SDM requires.
    EXPECT_EQ(m_memory->read_u32(kUnaligned).value_or(0), 0x80000010u);
}

TEST_F(JitLockPrefixTest, LockAddMem16RegUnaligned) {
    // 66 F0 01 0D 01200000 = LOCK ADD WORD PTR [0x2001], CX
    check_parity(
        {0x66, 0xF0, 0x01, 0x0D, 0x01, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            ASSERT_TRUE(m_memory->write_u16(kUnaligned, 0xFFF0u).is_ok());
            cpu.gpr.set16(ECX, 0x0020u);
        });
    // 16-bit wrap: only the low word is written, the byte at 0x2003 must be
    // untouched (check_parity compares the whole region for that).
    EXPECT_EQ(m_memory->read_u16(kUnaligned).value_or(0xFFFF), 0x0010u);
}

TEST_F(JitLockPrefixTest, LockXaddMem32RegUnaligned) {
    // F0 0F C1 0D 01200000 = LOCK XADD DWORD PTR [0x2001], ECX
    // Intel SDM Vol. 2 XADD: dest = dest + src, src = old dest.
    check_parity(
        {0xF0, 0x0F, 0xC1, 0x0D, 0x01, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            ASSERT_TRUE(m_memory->write_u32(kUnaligned, 100).is_ok());
            cpu.gpr.set32(ECX, 20);
        });
    EXPECT_EQ(m_memory->read_u32(kUnaligned).value_or(0), 120u);
}

TEST_F(JitLockPrefixTest, LockXaddMem16RegUnaligned) {
    // 66 F0 0F C1 0D 01200000 = LOCK XADD WORD PTR [0x2001], CX
    check_parity(
        {0x66, 0xF0, 0x0F, 0xC1, 0x0D, 0x01, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            ASSERT_TRUE(m_memory->write_u16(kUnaligned, 0x1234u).is_ok());
            cpu.gpr.set16(ECX, 0x1111u);
        });
    EXPECT_EQ(m_memory->read_u16(kUnaligned).value_or(0), 0x2345u);
}

TEST_F(JitLockPrefixTest, LockCmpxchgMem32UnalignedMatch) {
    // F0 0F B1 0D 01200000 = LOCK CMPXCHG DWORD PTR [0x2001], ECX
    check_parity(
        {0xF0, 0x0F, 0xB1, 0x0D, 0x01, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            ASSERT_TRUE(m_memory->write_u32(kUnaligned, 0xDEADu).is_ok());
            cpu.gpr.set32(EAX, 0xDEADu);
            cpu.gpr.set32(ECX, 0xBEEFu);
        });
    EXPECT_EQ(m_memory->read_u32(kUnaligned).value_or(0), 0xBEEFu);
}

TEST_F(JitLockPrefixTest, LockCmpxchgMem32UnalignedMismatch) {
    // Mismatch: memory unchanged, EAX loaded with the observed value, ZF=0.
    check_parity(
        {0xF0, 0x0F, 0xB1, 0x0D, 0x01, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            ASSERT_TRUE(m_memory->write_u32(kUnaligned, 0x1234u).is_ok());
            cpu.gpr.set32(EAX, 0x5678u);
            cpu.gpr.set32(ECX, 0xBEEFu);
        });
    EXPECT_EQ(m_memory->read_u32(kUnaligned).value_or(0), 0x1234u);
}

TEST_F(JitLockPrefixTest, LockCmpxchgMem16UnalignedMismatch) {
    // 66 F0 0F B1 0D 01200000 = LOCK CMPXCHG WORD PTR [0x2001], CX
    check_parity(
        {0x66, 0xF0, 0x0F, 0xB1, 0x0D, 0x01, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            ASSERT_TRUE(m_memory->write_u16(kUnaligned, 0x1234u).is_ok());
            cpu.gpr.set16(EAX, 0x5678u);
            cpu.gpr.set16(ECX, 0xBEEFu);
        });
    EXPECT_EQ(m_memory->read_u16(kUnaligned).value_or(0), 0x1234u);
}

TEST_F(JitLockPrefixTest, XchgMem32UnalignedUsesImplicitLockSemantics) {
    // 87 0D 01200000 = XCHG DWORD PTR [0x2001], ECX (LOCK# implicit).
    check_parity(
        {0x87, 0x0D, 0x01, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            ASSERT_TRUE(m_memory->write_u32(kUnaligned, 0x11223344u).is_ok());
            cpu.gpr.set32(ECX, 0x55667788u);
        });
    EXPECT_EQ(m_memory->read_u32(kUnaligned).value_or(0), 0x55667788u);
}

TEST_F(JitLockPrefixTest, LockOrMem32RegUnaligned) {
    // F0 09 0D 01200000 = LOCK OR DWORD PTR [0x2001], ECX
    check_parity(
        {0xF0, 0x09, 0x0D, 0x01, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            ASSERT_TRUE(m_memory->write_u32(kUnaligned, 0x00F0u).is_ok());
            cpu.gpr.set32(ECX, 0x0F00u);
        });
    EXPECT_EQ(m_memory->read_u32(kUnaligned).value_or(0), 0x0FF0u);
}

TEST_F(JitLockPrefixTest, LockAndMem32RegUnaligned) {
    // F0 21 0D 01200000 = LOCK AND DWORD PTR [0x2001], ECX
    check_parity(
        {0xF0, 0x21, 0x0D, 0x01, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            ASSERT_TRUE(m_memory->write_u32(kUnaligned, 0xFFFFu).is_ok());
            cpu.gpr.set32(ECX, 0x0F0Fu);
        });
    EXPECT_EQ(m_memory->read_u32(kUnaligned).value_or(0), 0x0F0Fu);
}

TEST_F(JitLockPrefixTest, LockOrMem16Imm8Unaligned) {
    // 66 F0 83 0D 01200000 01 = LOCK OR WORD PTR [0x2001], 1
    check_parity(
        {0x66, 0xF0, 0x83, 0x0D, 0x01, 0x20, 0x00, 0x00, 0x01},
        [this](CpuState&) {
            ASSERT_TRUE(m_memory->write_u16(kUnaligned, 0x0002u).is_ok());
        });
    EXPECT_EQ(m_memory->read_u16(kUnaligned).value_or(0), 0x0003u);
}

TEST_F(JitLockPrefixTest, LockAndMem16Imm8Unaligned) {
    // 66 F0 83 25 01200000 FE = LOCK AND WORD PTR [0x2001], 0xFFFE
    check_parity(
        {0x66, 0xF0, 0x83, 0x25, 0x01, 0x20, 0x00, 0x00, 0xFE},
        [this](CpuState&) {
            ASSERT_TRUE(m_memory->write_u16(kUnaligned, 0x0003u).is_ok());
        });
    EXPECT_EQ(m_memory->read_u16(kUnaligned).value_or(0), 0x0002u);
}

// Intel SDM Vol. 2 LOCK: the prefix is architecturally valid with BTS, BTR and
// BTC (and #UD with plain BT, which is why BT is not exercised here). The bit
// string is byte-addressed at base + (index >> 3), so an odd base and an index
// past 8 land the RMW on a byte the operand address does not name.
TEST_F(JitLockPrefixTest, LockBtsMem32Imm8Unaligned) {
    // F0 0F BA 2D 01200000 0B = LOCK BTS DWORD PTR [0x2001], 11
    check_parity(
        {0xF0, 0x0F, 0xBA, 0x2D, 0x01, 0x20, 0x00, 0x00, 0x0B},
        [this](CpuState&) {
            ASSERT_TRUE(m_memory->write_u32(kUnaligned, 0x00000000u).is_ok());
        });
    // bit 11 of the dword at 0x2001 -> byte 0x2002, bit 3.
    EXPECT_EQ(m_memory->read_u32(kUnaligned).value_or(0), 0x00000800u);
}

TEST_F(JitLockPrefixTest, LockBtrMem32Imm8Unaligned) {
    // F0 0F BA 35 01200000 0B = LOCK BTR DWORD PTR [0x2001], 11
    check_parity(
        {0xF0, 0x0F, 0xBA, 0x35, 0x01, 0x20, 0x00, 0x00, 0x0B},
        [this](CpuState&) {
            ASSERT_TRUE(m_memory->write_u32(kUnaligned, 0xFFFFFFFFu).is_ok());
        });
    EXPECT_EQ(m_memory->read_u32(kUnaligned).value_or(0), 0xFFFFF7FFu);
}

TEST_F(JitLockPrefixTest, LockBtcMem32Imm8Unaligned) {
    // F0 0F BA 3D 01200000 0B = LOCK BTC DWORD PTR [0x2001], 11
    check_parity(
        {0xF0, 0x0F, 0xBA, 0x3D, 0x01, 0x20, 0x00, 0x00, 0x0B},
        [this](CpuState&) {
            ASSERT_TRUE(m_memory->write_u32(kUnaligned, 0x00000800u).is_ok());
        });
    EXPECT_EQ(m_memory->read_u32(kUnaligned).value_or(0xFFFF), 0x00000000u);
}

TEST_F(JitLockPrefixTest, LockBtsMem32RegUnalignedPastOperandByte) {
    // F0 0F AB 0D 01200000 = LOCK BTS DWORD PTR [0x2001], ECX with ECX = 100.
    // Bit 100 is byte 0x2001 + 12 = 0x200D, bit 4 — outside the four bytes the
    // operand names, which is the case a byte-offset truncation would miss.
    check_parity(
        {0xF0, 0x0F, 0xAB, 0x0D, 0x01, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            ASSERT_TRUE(m_memory->write_u8(0x200D, 0x00).is_ok());
            cpu.gpr.set32(ECX, 100);
        });
    EXPECT_EQ(m_memory->read_u8(0x200D).value_or(0xFF), 0x10u);
}

// Intel SDM Vol. 2 CMPXCHG8B: unlike CMPXCHG16B, the m64 operand carries NO
// alignment requirement, so an unaligned CMPXCHG8B must complete, not fault.
TEST_F(JitLockPrefixTest, LockCmpxchg8bUnalignedDoesNotFault) {
    // F0 0F C7 0D 01200000 = LOCK CMPXCHG8B [0x2001]
    check_parity(
        {0xF0, 0x0F, 0xC7, 0x0D, 0x01, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            ASSERT_TRUE(m_memory->write_u32(kUnaligned, 0xAAAAAAAAu).is_ok());
            ASSERT_TRUE(m_memory->write_u32(kUnaligned + 4,
                                            0xBBBBBBBBu).is_ok());
            cpu.gpr.set32(EAX, 0xAAAAAAAAu);
            cpu.gpr.set32(EDX, 0xBBBBBBBBu);
            cpu.gpr.set32(EBX, 0x11111111u);
            cpu.gpr.set32(ECX, 0x22222222u);
        });
    EXPECT_EQ(m_memory->read_u32(kUnaligned).value_or(0), 0x11111111u);
    EXPECT_EQ(m_memory->read_u32(kUnaligned + 4).value_or(0), 0x22222222u);
}

TEST_F(JitLockPrefixTest, LockCmpxchg8bUnalignedMismatchDoesNotFault) {
    check_parity(
        {0xF0, 0x0F, 0xC7, 0x0D, 0x01, 0x20, 0x00, 0x00},
        [this](CpuState& cpu) {
            ASSERT_TRUE(m_memory->write_u32(kUnaligned, 0x11111111u).is_ok());
            ASSERT_TRUE(m_memory->write_u32(kUnaligned + 4,
                                            0x22222222u).is_ok());
            cpu.gpr.set32(EAX, 0xFFFFFFFFu);
            cpu.gpr.set32(EDX, 0xFFFFFFFFu);
            cpu.gpr.set32(EBX, 0xDEADBEEFu);
            cpu.gpr.set32(ECX, 0xCAFEBABEu);
        });
    EXPECT_EQ(m_memory->read_u32(kUnaligned).value_or(0), 0x11111111u);
    EXPECT_EQ(m_memory->read_u32(kUnaligned + 4).value_or(0), 0x22222222u);
}

// ---------------------------------------------------------------------------
// 8-byte forms: the r/m64 helpers are 64-bit-only lowerings, so drive them
// directly the way the cross-process atomicity tests above do.
// ---------------------------------------------------------------------------

TEST_F(JitLockPrefixTest, LockAdd64Unaligned) {
    CpuState cpu;
    cpu.init_long_mode();
    ASSERT_TRUE(m_memory->write_u64(kUnaligned, 0x00000000FFFFFFFFull).is_ok());
    jit_lock_add64(&cpu, m_memory.get(), kUnaligned, 1);
    cpu.materialize_lazy_flags();
    ASSERT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(m_memory->read_u64(kUnaligned).value_or(0), 0x100000000ull);
    EXPECT_FALSE(cpu.flags.zf());
    EXPECT_FALSE(cpu.flags.cf());
    EXPECT_FALSE(cpu.flags.sf());
}

TEST_F(JitLockPrefixTest, LockXadd64Unaligned) {
    CpuState cpu;
    cpu.init_long_mode();
    ASSERT_TRUE(m_memory->write_u64(kUnaligned, 100).is_ok());
    cpu.gpr.set64(ECX, 20);
    jit_lock_xadd64(&cpu, m_memory.get(), kUnaligned, ECX);
    ASSERT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(m_memory->read_u64(kUnaligned).value_or(0), 120u);
    EXPECT_EQ(cpu.gpr.get64(ECX), 100u) << "XADD must return the old dest";
}

TEST_F(JitLockPrefixTest, LockCmpxchg64UnalignedMatch) {
    CpuState cpu;
    cpu.init_long_mode();
    ASSERT_TRUE(m_memory->write_u64(kUnaligned, 0xDEADBEEFCAFEBABEull).is_ok());
    cpu.gpr.set64(EAX, 0xDEADBEEFCAFEBABEull);
    jit_lock_cmpxchg64(&cpu, m_memory.get(), kUnaligned, 0x1111222233334444ull);
    cpu.materialize_lazy_flags();
    ASSERT_FALSE(cpu.pending_raise.active);
    EXPECT_TRUE(cpu.flags.zf());
    EXPECT_EQ(m_memory->read_u64(kUnaligned).value_or(0),
              0x1111222233334444ull);
}

TEST_F(JitLockPrefixTest, LockCmpxchg64UnalignedMismatch) {
    CpuState cpu;
    cpu.init_long_mode();
    ASSERT_TRUE(m_memory->write_u64(kUnaligned, 0xDEADBEEFCAFEBABEull).is_ok());
    cpu.gpr.set64(EAX, 1);
    jit_lock_cmpxchg64(&cpu, m_memory.get(), kUnaligned, 0x1111222233334444ull);
    cpu.materialize_lazy_flags();
    ASSERT_FALSE(cpu.pending_raise.active);
    EXPECT_FALSE(cpu.flags.zf());
    EXPECT_EQ(cpu.gpr.get64(EAX), 0xDEADBEEFCAFEBABEull);
    EXPECT_EQ(m_memory->read_u64(kUnaligned).value_or(0),
              0xDEADBEEFCAFEBABEull) << "failed CMPXCHG must not store";
}

TEST_F(JitLockPrefixTest, AtomicXchg64Unaligned) {
    CpuState cpu;
    cpu.init_long_mode();
    ASSERT_TRUE(m_memory->write_u64(kUnaligned, 0x1122334455667788ull).is_ok());
    cpu.gpr.set64(ECX, 0x99AABBCCDDEEFF00ull);
    jit_atomic_xchg64(&cpu, m_memory.get(), kUnaligned, ECX);
    ASSERT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(m_memory->read_u64(kUnaligned).value_or(0),
              0x99AABBCCDDEEFF00ull);
    EXPECT_EQ(cpu.gpr.get64(ECX), 0x1122334455667788ull);
}

TEST_F(JitLockPrefixTest, LockOrAnd64Unaligned) {
    CpuState cpu;
    cpu.init_long_mode();
    ASSERT_TRUE(m_memory->write_u64(kUnaligned, 0x00FF00FF00FF00FFull).is_ok());
    jit_lock_or64(&cpu, m_memory.get(), kUnaligned, 0xFF00000000000000ull);
    cpu.materialize_lazy_flags();
    ASSERT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(m_memory->read_u64(kUnaligned).value_or(0),
              0xFFFF00FF00FF00FFull);
    // Intel SDM Vol. 2 OR: SF from the result's top bit, CF and OF cleared.
    EXPECT_TRUE(cpu.flags.sf());
    EXPECT_FALSE(cpu.flags.cf());
    EXPECT_FALSE(cpu.flags.of());

    jit_lock_and64(&cpu, m_memory.get(), kUnaligned, 0x0000000000000000ull);
    cpu.materialize_lazy_flags();
    EXPECT_EQ(m_memory->read_u64(kUnaligned).value_or(1), 0u);
    EXPECT_TRUE(cpu.flags.zf());
}

TEST_F(JitLockPrefixTest, LockBtFamily64Unaligned) {
    CpuState cpu;
    cpu.init_long_mode();
    ASSERT_TRUE(m_memory->write_u64(kUnaligned, 0).is_ok());

    // Bit 70 of the string based at 0x2001 is byte 0x2001 + 8 = 0x2009, bit 6.
    jit_lock_bts64(&cpu, m_memory.get(), kUnaligned, 70);
    cpu.materialize_lazy_flags();
    EXPECT_FALSE(cpu.flags.cf()) << "BTS reports the PREVIOUS bit in CF";
    EXPECT_EQ(m_memory->read_u8(0x2009).value_or(0), 0x40u);

    jit_lock_bts64(&cpu, m_memory.get(), kUnaligned, 70);
    cpu.materialize_lazy_flags();
    EXPECT_TRUE(cpu.flags.cf());

    jit_lock_btc64(&cpu, m_memory.get(), kUnaligned, 70);
    cpu.materialize_lazy_flags();
    EXPECT_TRUE(cpu.flags.cf());
    EXPECT_EQ(m_memory->read_u8(0x2009).value_or(0xFF), 0x00u);

    jit_lock_btr64(&cpu, m_memory.get(), kUnaligned, 70);
    cpu.materialize_lazy_flags();
    EXPECT_FALSE(cpu.flags.cf());
    EXPECT_EQ(m_memory->read_u8(0x2009).value_or(0xFF), 0x00u);
    ASSERT_FALSE(cpu.pending_raise.active);
}

TEST_F(JitLockPrefixTest, LockCmpxchg8bUnalignedHelperDoesNotFault) {
    CpuState cpu;
    cpu.init_protected_mode();
    ASSERT_TRUE(m_memory->write_u32(kUnaligned, 0xAAAAAAAAu).is_ok());
    ASSERT_TRUE(m_memory->write_u32(kUnaligned + 4, 0xBBBBBBBBu).is_ok());
    cpu.gpr.set32(EAX, 0xAAAAAAAAu);
    cpu.gpr.set32(EDX, 0xBBBBBBBBu);
    cpu.gpr.set32(EBX, 0x11111111u);
    cpu.gpr.set32(ECX, 0x22222222u);
    jit_lock_cmpxchg8b(&cpu, m_memory.get(), kUnaligned);
    cpu.materialize_lazy_flags();
    EXPECT_FALSE(cpu.pending_raise.active)
        << "CMPXCHG8B has no alignment requirement (Intel SDM Vol. 2)";
    EXPECT_TRUE(cpu.flags.zf());
    EXPECT_EQ(m_memory->read_u32(kUnaligned).value_or(0), 0x11111111u);
    EXPECT_EQ(m_memory->read_u32(kUnaligned + 4).value_or(0), 0x22222222u);
}

// Intel SDM Vol. 2 CMPXCHG16B: the m128 operand MUST be 16-byte aligned;
// otherwise #GP(0). Vitreo surfaces that as STATUS_ACCESS_VIOLATION, the same
// mapping the interpreter uses (tests/cpu/test_atomics_misc.cpp
// LockCmpxchg16bUnalignedRaisesGp).
TEST_F(JitLockPrefixTest, LockCmpxchg16bUnalignedRaisesAccessViolation) {
    CpuState cpu;
    cpu.init_long_mode();
    ASSERT_TRUE(m_memory->write_u64(kUnaligned, 0).is_ok());
    ASSERT_TRUE(m_memory->write_u64(kUnaligned + 8, 0).is_ok());
    cpu.gpr.set64(EAX, 0);
    cpu.gpr.set64(EDX, 0);
    cpu.gpr.set64(EBX, 0xDEADBEEFull);
    cpu.gpr.set64(ECX, 0xCAFEBABEull);
    jit_lock_cmpxchg16b(&cpu, m_memory.get(), kUnaligned);
    EXPECT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.args[1], kUnaligned);
    EXPECT_EQ(m_memory->read_u64(kUnaligned).value_or(1), 0u)
        << "an unaligned CMPXCHG16B must never store non-atomically";
}

// ---------------------------------------------------------------------------
// Write-watch: the helpers call record_store() BEFORE taking the mutex, and an
// unaligned operand can straddle two guest pages. A GC reading GetWriteWatch
// treats an unmarked page as clean and frees live objects
// (getwritewatch_gc_landmine), so both pages must be marked.
// ---------------------------------------------------------------------------

TEST_F(JitLockPrefixTest, UnalignedLockAdd32SpanningPagesMarksBothPages) {
    // 0x2FFE + 4 bytes runs into the 0x3000 page. The watch is registered
    // AFTER the setup store so only the helper's record_store can mark it.
    constexpr GuestAddr split = 0x2FFE;
    ASSERT_TRUE(m_memory->write_u32(split, 1).is_ok());
    WriteWatchScope watch(0x1000, 0x10000);
    CpuState cpu;
    cpu.init_protected_mode();
    jit_lock_add32(&cpu, m_memory.get(), split, 41);
    ASSERT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(m_memory->read_u32(split).value_or(0), 42u);
    EXPECT_TRUE(watch.page_dirty(0x2000));
    EXPECT_TRUE(watch.page_dirty(0x3000))
        << "the tail of a page-straddling locked RMW was reported clean";
}

TEST_F(JitLockPrefixTest, UnalignedLockCmpxchg64MarksBothStraddledPages) {
    constexpr GuestAddr split = 0x2FFD;  // 8 bytes: 0x2FFD..0x3004
    ASSERT_TRUE(m_memory->write_u64(split, 7).is_ok());
    WriteWatchScope watch(0x1000, 0x10000);
    CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.set64(EAX, 7);
    jit_lock_cmpxchg64(&cpu, m_memory.get(), split, 9);
    ASSERT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(m_memory->read_u64(split).value_or(0), 9u);
    EXPECT_TRUE(watch.page_dirty(0x2000));
    EXPECT_TRUE(watch.page_dirty(0x3000));
}

TEST_F(JitLockPrefixTest, UnalignedLockXchg32MarksTheWrittenPage) {
    ASSERT_TRUE(m_memory->write_u32(kUnaligned, 0x11111111u).is_ok());
    WriteWatchScope watch(0x1000, 0x10000);
    CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.set32(ECX, 0x22222222u);
    jit_atomic_xchg32(&cpu, m_memory.get(), kUnaligned, ECX);
    ASSERT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(m_memory->read_u32(kUnaligned).value_or(0), 0x22222222u);
    EXPECT_EQ(cpu.gpr.get32(ECX), 0x11111111u);
    EXPECT_TRUE(watch.page_dirty(0x2000));
    EXPECT_FALSE(watch.page_dirty(0x4000))
        << "record_store must not mark pages the RMW never touched";
}

// The bit string byte is base + (index >> 3): a large index moves the store to
// a different PAGE than the operand address, and only that page may be marked.
TEST_F(JitLockPrefixTest, UnalignedLockBts64MarksTheComputedBytesPage) {
    // bit 0x8000 of the string based at 0x2001 -> byte 0x2001 + 0x1000.
    constexpr i64 bit = 0x8000;
    constexpr GuestAddr touched = kUnaligned + (bit / 8);
    ASSERT_TRUE(m_memory->write_u8(touched, 0).is_ok());
    WriteWatchScope watch(0x1000, 0x10000);
    CpuState cpu;
    cpu.init_long_mode();
    jit_lock_bts64(&cpu, m_memory.get(), kUnaligned, bit);
    ASSERT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(m_memory->read_u8(touched).value_or(0xFF), 0x01u);
    EXPECT_TRUE(watch.page_dirty(touched & ~0xFFFull));
}

// Guards the premise of every unaligned test above: an odd guest address must
// produce an odd host pointer, so host_ptr_mut's alignment test fails and the
// helpers really do take the split-lock fallback rather than the atomic_ref
// fast path. If the mapping ever stopped preserving the low bits, the tests
// would silently go back to covering only the fast path.
TEST_F(JitLockPrefixTest, UnalignedGuestAddressReachesTheSplitLockFallback) {
    Byte* host = m_memory->host_ptr_mut(kUnaligned, sizeof(u64));
    ASSERT_NE(host, nullptr);
    EXPECT_NE(reinterpret_cast<uintptr_t>(host) % alignof(u64), 0u);
    EXPECT_NE(reinterpret_cast<uintptr_t>(host) % alignof(u32), 0u);
    EXPECT_NE(reinterpret_cast<uintptr_t>(host) % alignof(u16), 0u);
}

// Intel SDM Vol. 2 CMPXCHG8B / Vol. 3 paging: an unreadable m64 operand faults.
// The unlocked helper used to `return` silently on the failed read, leaving ZF
// and EDX:EAX holding the previous instruction's values -- a guest CAS loop
// then reads a stale answer as a completed exchange.
TEST_F(JitLockPrefixTest, UnlockedCmpxchg8bUnmappedRaisesAccessViolation) {
    CpuState cpu;
    cpu.init_protected_mode();
    cpu.flags.set_zf(true);
    cpu.gpr.set32(EAX, 0xAAAAAAAAu);
    cpu.gpr.set32(EDX, 0xBBBBBBBBu);
    jit_cmpxchg8b(&cpu, m_memory.get(), 0xC);
    EXPECT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.num_args, 2u);
    EXPECT_EQ(cpu.pending_raise.args[1], 0xCu);
}

TEST_F(JitLockPrefixTest, LockedCmpxchg8bUnmappedRaisesAccessViolation) {
    CpuState cpu;
    cpu.init_protected_mode();
    jit_lock_cmpxchg8b(&cpu, m_memory.get(), 0xC);
    EXPECT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.args[1], 0xCu);
}

// ============================================================================
// LockPrefixGuard protocol
//
// x86 makes every LOCK-prefixed RMW atomic, including a split lock. Vitreo
// serves the aligned host-mapped case with one host atomic and everything else
// with a non-atomic RMW through GuestMemory, so only the second kind needs
// mutual exclusion. These tests pin both halves of that: the fast path must
// take no exclusive lock at all (that was 24.7% of non-parked time in a
// 32-thread Battle.net renderer), and the fallback must still exclude it.
// ============================================================================

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <thread>
#include "core/cpu/lock_prefix.hpp"

namespace {

bool mutex_is_held_from_another_thread() {
    // std::mutex::try_lock from the thread that already owns it is UB, so ask
    // a fresh thread.
    bool held = false;
    std::thread probe([&] {
        if (vitreo::cpu::lock_prefix_mutex().try_lock()) {
            vitreo::cpu::lock_prefix_mutex().unlock();
            held = false;
        } else {
            held = true;
        }
    });
    probe.join();
    return held;
}

}  // namespace

// The whole point of the change: an aligned, host-mapped operand is served by
// a host atomic, so it must not serialize on the process-wide mutex.
TEST(LockPrefixGuardTest, AtomicOperandTakesNoExclusiveLock) {
    if (vitreo::cpu::lock_prefix_force_exclusive()) {
        GTEST_SKIP() << "VITREO_LOCK_PREFIX_EXCLUSIVE forces the path this "
                        "test asserts is not taken";
    }
    alignas(4) std::uint32_t word = 0;
    {
        vitreo::cpu::LockPrefixGuard guard(&word, sizeof(word));
        EXPECT_FALSE(mutex_is_held_from_another_thread());
    }
    EXPECT_FALSE(mutex_is_held_from_another_thread());
}

TEST(LockPrefixGuardTest, PredecodedAlignedCmpxchgDoesNotQueueOnExclusiveMutex) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x3000,
                                  MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u32(0x2000, 7).is_ok());
    const u8 code[] = {0xF0, 0x0F, 0xB1, 0x0D, 0x00, 0x20, 0x00, 0x00};
    MemoryByteReader reader(code, sizeof(code));
    DecodedInstruction instr;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode32, instr),
              DecodeError::Success);

    std::atomic<bool> mutex_held{false};
    std::atomic<bool> release_mutex{false};
    std::thread holder([&] {
        std::lock_guard<std::mutex> lock(lock_prefix_mutex());
        mutex_held.store(true, std::memory_order_release);
        while (!release_mutex.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    while (!mutex_held.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    CpuState cpu;
    cpu.init_protected_mode();
    cpu.rip = 0x1000;
    cpu.gpr.set32(EAX, 7);
    cpu.gpr.set32(ECX, 9);
    Interpreter interpreter(cpu, memory);
    std::atomic<bool> completed{false};
    std::thread guest([&] {
        EXPECT_EQ(interpreter.execute_predecoded(instr), ExecResult::Continue);
        completed.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(100);
    while (!completed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_TRUE(completed.load(std::memory_order_acquire))
        << "aligned CMPXCHG queued behind an unrelated split-lock fallback";
    release_mutex.store(true, std::memory_order_release);
    holder.join();
    guest.join();
    EXPECT_EQ(memory.read_u32(0x2000).value(), 9u);
}

TEST(LockPrefixGuardTest, PredecodedByteOrDoesNotQueueOnExclusiveMutex) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x3000,
                                  MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u8(0x2000, 1).is_ok());
    const u8 code[] = {0xF0, 0x08, 0x0D, 0x00, 0x20, 0x00, 0x00};
    MemoryByteReader reader(code, sizeof(code));
    DecodedInstruction instr;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode32, instr),
              DecodeError::Success);

    std::atomic<bool> mutex_held{false};
    std::atomic<bool> release_mutex{false};
    std::thread holder([&] {
        std::lock_guard<std::mutex> lock(lock_prefix_mutex());
        mutex_held.store(true, std::memory_order_release);
        while (!release_mutex.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    while (!mutex_held.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    CpuState cpu;
    cpu.init_protected_mode();
    cpu.rip = 0x1000;
    cpu.gpr.set32(EAX, 7);
    cpu.gpr.set32(ECX, 0x80);
    Interpreter interpreter(cpu, memory);
    std::atomic<bool> completed{false};
    std::thread guest([&] {
        EXPECT_EQ(interpreter.execute_predecoded(instr), ExecResult::Continue);
        completed.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(100);
    while (!completed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_TRUE(completed.load(std::memory_order_acquire))
        << "byte OR queued behind an unrelated split-lock fallback";
    release_mutex.store(true, std::memory_order_release);
    holder.join();
    guest.join();
    EXPECT_EQ(memory.read_u8(0x2000).value(), 0x81u);
}

// A split lock cannot use a host atomic, so it must take the exclusive lock.
TEST(LockPrefixGuardTest, SplitOperandTakesTheExclusiveLock) {
    alignas(4) std::uint8_t bytes[8] = {};
    void* misaligned = &bytes[1];
    {
        vitreo::cpu::LockPrefixGuard guard(misaligned, sizeof(std::uint32_t));
        EXPECT_TRUE(mutex_is_held_from_another_thread());
    }
    EXPECT_FALSE(mutex_is_held_from_another_thread());
}

// An unmapped operand (no host pointer) is served by the GuestMemory fallback
// and needs the same exclusion as a split lock.
TEST(LockPrefixGuardTest, NullOperandTakesTheExclusiveLock) {
    vitreo::cpu::LockPrefixGuard guard(nullptr, sizeof(std::uint32_t));
    EXPECT_TRUE(mutex_is_held_from_another_thread());
}

// The contract the fast path buys with its fence: no thread may be inside a
// host-atomic LOCK while a fallback is inside its non-atomic RMW, in either
// order of arrival.
TEST(LockPrefixGuardTest, FallbackAndAtomicNeverOverlap) {
    if (vitreo::cpu::lock_prefix_force_exclusive()) {
        GTEST_SKIP() << "VITREO_LOCK_PREFIX_EXCLUSIVE forces the path this "
                        "test asserts is not taken";
    }
    alignas(4) std::uint32_t word = 0;
    std::atomic<int> atomics_inside{0};
    std::atomic<bool> fallback_inside{false};
    std::atomic<bool> violation{false};
    std::atomic<bool> stop{false};
    std::atomic<long> reader_iterations{0};

    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                vitreo::cpu::LockPrefixGuard guard(&word, sizeof(word));
                atomics_inside.fetch_add(1, std::memory_order_seq_cst);
                if (fallback_inside.load(std::memory_order_seq_cst)) {
                    violation.store(true, std::memory_order_relaxed);
                }
                atomics_inside.fetch_sub(1, std::memory_order_seq_cst);
                reader_iterations.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (int round = 0; round < 2000; ++round) {
        vitreo::cpu::LockPrefixGuard guard(nullptr, sizeof(word));
        fallback_inside.store(true, std::memory_order_seq_cst);
        if (atomics_inside.load(std::memory_order_seq_cst) != 0) {
            violation.store(true, std::memory_order_relaxed);
        }
        fallback_inside.store(false, std::memory_order_seq_cst);
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& t : readers) t.join();
    EXPECT_FALSE(violation.load()) << "a host-atomic LOCK overlapped a "
                                      "non-atomic fallback RMW";
    // Guard against the test passing vacuously because the readers never ran.
    EXPECT_GT(reader_iterations.load(), 1000);
}

// The guard is re-entrant because a fallback can be reached from a dispatcher
// that already holds it. A nested fallback under an outer fast path must
// upgrade the thread rather than run unprotected, and must not deadlock
// against the writer-quiesce spin.
TEST(LockPrefixGuardTest, NestedFallbackUnderAtomicUpgradesAndReleases) {
    if (vitreo::cpu::lock_prefix_force_exclusive()) {
        GTEST_SKIP() << "VITREO_LOCK_PREFIX_EXCLUSIVE forces the path this "
                        "test asserts is not taken";
    }
    alignas(4) std::uint32_t word = 0;
    {
        vitreo::cpu::LockPrefixGuard outer(&word, sizeof(word));
        EXPECT_FALSE(mutex_is_held_from_another_thread());
        {
            vitreo::cpu::LockPrefixGuard inner(nullptr, sizeof(word));
            EXPECT_TRUE(mutex_is_held_from_another_thread());
        }
        // The inner guard is gone but the thread stays exclusive: the
        // outermost guard owns the release.
        EXPECT_TRUE(mutex_is_held_from_another_thread());
    }
    EXPECT_FALSE(mutex_is_held_from_another_thread());
}

// Nesting in the other order, and the plain nested case, must both be no-ops
// that leave nothing locked behind.
TEST(LockPrefixGuardTest, NestedGuardsReleaseCleanly) {
    if (vitreo::cpu::lock_prefix_force_exclusive()) {
        GTEST_SKIP() << "VITREO_LOCK_PREFIX_EXCLUSIVE forces the path this "
                        "test asserts is not taken";
    }
    alignas(4) std::uint32_t word = 0;
    {
        vitreo::cpu::LockPrefixGuard outer(nullptr, sizeof(word));
        vitreo::cpu::LockPrefixGuard inner(&word, sizeof(word));
        EXPECT_TRUE(mutex_is_held_from_another_thread());
    }
    EXPECT_FALSE(mutex_is_held_from_another_thread());
    {
        vitreo::cpu::LockPrefixGuard outer(&word, sizeof(word));
        vitreo::cpu::LockPrefixGuard inner(&word, sizeof(word));
        EXPECT_FALSE(mutex_is_held_from_another_thread());
    }
    EXPECT_FALSE(mutex_is_held_from_another_thread());
}

// ============================================================================
// Contended-LOCK throughput. DISABLED by default: it is a measurement, not an
// assertion, and its result depends on the host's core count and load.
//
// Run both ways to A/B the fast path against the old always-exclusive mutex:
//   ./vitreo_cpu_tests --gtest_also_run_disabled_tests \
//       --gtest_filter='*LockThroughput*'
//   VITREO_LOCK_PREFIX_EXCLUSIVE=1 ./vitreo_cpu_tests ... (same)
// ============================================================================
TEST(LockPrefixGuardTest, DISABLED_LockThroughputUnderContention) {
    const char* thread_env = std::getenv("VITREO_BENCH_THREADS");
    const int kThreads = thread_env != nullptr ? std::atoi(thread_env) : 16;
    const char* iter_env = std::getenv("VITREO_BENCH_ITERS");
    const int kIters = iter_env != nullptr ? std::atoi(iter_env) : 200000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x1000, 0x10000,
                                  MemoryProtection::ReadWrite).is_ok());
    // Each thread gets its own naturally aligned word, so the guest-visible
    // work is uncontended: anything we measure is Vitreo's own serialization,
    // not cache-line contention between the guest's own atomics.
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            CpuState cpu;
            cpu.init_protected_mode();
            const u64 addr = 0x2000 + static_cast<u64>(t) * 64;
            while (!go.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < kIters; ++i) {
                cpu.gpr.set32(static_cast<u8>(RegIndex::RAX),
                              static_cast<u32>(i));
                jit_lock_cmpxchg32(&cpu, &memory, addr,
                                   static_cast<u32>(i) + 1u);
            }
        });
    }
    const auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double ms =
        std::chrono::duration<double, std::milli>(elapsed).count();
    const double total = static_cast<double>(kThreads) * kIters;
    std::printf("[bench] %d threads x %d lock cmpxchg32 : %.1f ms, "
                "%.2f M ops/s, exclusive=%d\n",
                kThreads, kIters, ms, total / ms / 1000.0,
                vitreo::cpu::lock_prefix_force_exclusive() ? 1 : 0);
}

TEST(LockPrefixGuardTest, ByteOrPreservesFlagsWidthsAndWriteWatch) {
    for (bool mode64 : {false, true}) {
        for (bool high_byte : {false, true}) {
            for (u8 initial : {u8{0}, u8{1}, u8{0x80}, u8{0xFE}}) {
                GuestMemory memory;
                const GuestAddr addr = mode64 ? 0x120002001ull : 0x2001;
                ASSERT_TRUE(memory.map_region(addr - 1, 0x1000, MemoryProtection::ReadWrite));
                ASSERT_TRUE(memory.write_u32(addr - 1, 0xA55A0000u | (u32{initial} << 8) | 0x3C));
                WriteWatchScope watch(addr - 1, 0x1000);
                CpuState cpu;
                if (mode64) cpu.init_long_mode(); else cpu.init_protected_mode();
                cpu.gpr.set64(EBX, addr);
                cpu.gpr.set64(EAX, 0x0200); // AH = 2
                cpu.gpr.set64(ECX, 0x80);   // CL = 0x80
                cpu.flags.set_cf(true);
                cpu.flags.set_of(true);
                const u8 bytes[] = {0xF0, 0x08, static_cast<u8>(high_byte ? 0x23 : 0x0B)};
                MemoryByteReader reader(bytes, sizeof(bytes));
                DecodedInstruction instr;
                ASSERT_EQ(decode_instruction(reader, mode64 ? DecodeMode::Mode64 : DecodeMode::Mode32, instr), DecodeError::Success);
                Interpreter interpreter(cpu, memory);
                ASSERT_EQ(interpreter.execute_predecoded(instr), ExecResult::Continue);
                const u8 result = initial | (high_byte ? 2 : 0x80);
                EXPECT_EQ(memory.read_u32(addr - 1).value(), 0xA55A003Cu | (u32{result} << 8));
                EXPECT_FALSE(cpu.flags.cf()); EXPECT_FALSE(cpu.flags.of());
                EXPECT_EQ(cpu.flags.zf(), result == 0);
                EXPECT_EQ(cpu.flags.sf(), (result & 0x80) != 0);
                unsigned ones = 0;
                for (unsigned bit = 0; bit < 8; ++bit) ones += (result >> bit) & 1;
                EXPECT_EQ(cpu.flags.pf(), (ones & 1) == 0);
                EXPECT_EQ(cpu.gpr.get64(EAX), 0x0200u);
                EXPECT_EQ(cpu.gpr.get64(ECX), 0x80u);
                EXPECT_TRUE(watch.page_dirty(addr));
            }
        }
    }
}

TEST(LockPrefixGuardTest, ByteOrFaultPreservesMemoryAndFlags) {
    for (GuestAddr addr : {GuestAddr{0x2000}, GuestAddr{0x4000}}) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x2000, 0x1000, MemoryProtection::Read));
        CpuState cpu; cpu.init_protected_mode();
        cpu.gpr.set32(EAX, addr); cpu.gpr.set32(ECX, 0);
        cpu.flags.set_cf(true); cpu.flags.set_of(true);
        const u8 bytes[] = {0xF0, 0x08, 0x08};
        MemoryByteReader reader(bytes, sizeof(bytes)); DecodedInstruction instr;
        ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode32, instr), DecodeError::Success);
        Interpreter interpreter(cpu, memory);
        EXPECT_EQ(interpreter.execute_predecoded(instr), ExecResult::Exception);
        EXPECT_TRUE(cpu.flags.cf()); EXPECT_TRUE(cpu.flags.of());
        EXPECT_EQ(memory.read_u8(0x2000).value(), 0);
    }
}

TEST(LockPrefixGuardTest, ConcurrentByteOrDoesNotLoseBits) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x2000, 0x1000, MemoryProtection::ReadWrite));
    std::barrier phases(9);
    std::vector<std::thread> workers;
    std::atomic<bool> failed{false};
    const u8 bytes[] = {0xF0, 0x08, 0x08};
    MemoryByteReader reader(bytes, sizeof(bytes)); DecodedInstruction instr;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode32, instr), DecodeError::Success);
    for (unsigned bit = 0; bit < 8; ++bit) workers.emplace_back([&, bit] {
        CpuState cpu; cpu.init_protected_mode();
        cpu.gpr.set32(EAX, 0x2001); cpu.gpr.set32(ECX, 1u << bit);
        Interpreter interpreter(cpu, memory);
        for (unsigned round = 0; round < 1000; ++round) {
            phases.arrive_and_wait();
            if (interpreter.execute_predecoded(instr) != ExecResult::Continue) failed.store(true);
            phases.arrive_and_wait();
        }
    });
    for (unsigned round = 0; round < 1000; ++round) {
        EXPECT_TRUE(memory.write_u32(0x2000, 0xA55A003C));
        phases.arrive_and_wait(); phases.arrive_and_wait();
        EXPECT_EQ(memory.read_u32(0x2000).value(), 0xA55AFF3Cu);
    }
    for (auto& worker : workers) worker.join();
    EXPECT_FALSE(failed.load());
}

TEST_F(JitLockPrefixTest, ByteOrRegisterAndImmediateMatchInterpreter) {
    const std::vector<std::vector<u8>> forms = {
        {0xF0, 0x08, 0x0B},       // LOCK OR [EBX], CL
        {0xF0, 0x08, 0x23},       // LOCK OR [EBX], AH
        {0xF0, 0x80, 0x0B, 0x00}, // LOCK OR [EBX], 0 (still a write)
        {0xF0, 0x80, 0x0B, 0x80}, // LOCK OR [EBX], 0x80
    };
    for (const auto& code : forms) {
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        check_parity(code.data(), code.size(), [&](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x2001);
            cpu.gpr.set32(EAX, 0x0200);
            cpu.gpr.set32(ECX, 0x80);
            ASSERT_TRUE(m_memory->write_u32(0x2000, 0xA55A003C));
            cpu.flags.set_cf(true); cpu.flags.set_of(true);
        });
        const u32 bits = code.size() == 4 ? code.back() : (code[2] == 0x23 ? 2 : 0x80);
        EXPECT_EQ(m_memory->read_u32(0x2000).value(), 0xA55A003Cu | (bits << 8));
    }
}
