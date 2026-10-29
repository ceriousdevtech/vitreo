// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Tests for atomics/misc instructions: CMPXCHG, XADD, XCHG r/m, MOVSXD,
// CLC, STC, CMC, CLD, STD, LAHF, SAHF

#include <gtest/gtest.h>
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/jit_compiler.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

// ============================================================================
// Helpers
// ============================================================================

static DecodedInstruction decode(const std::initializer_list<u8>& bytes,
                                 DecodeMode mode = DecodeMode::Mode64) {
    std::vector<u8> data(bytes);
    MemoryByteReader reader(data.data(), data.size());
    DecodedInstruction inst;
    DecodeError err = decode_instruction(reader, mode, inst);
    EXPECT_EQ(err, DecodeError::Success) << "Decode failed";
    return inst;
}

constexpr u8 RAX = static_cast<u8>(RegIndex::RAX);
constexpr u8 RCX = static_cast<u8>(RegIndex::RCX);
constexpr u8 RDX = static_cast<u8>(RegIndex::RDX);
constexpr u8 RBX = static_cast<u8>(RegIndex::RBX);
constexpr u8 RSP = static_cast<u8>(RegIndex::RSP);
constexpr u8 RSI = static_cast<u8>(RegIndex::RSI);

class AtomicsMiscTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        auto map_result = m_memory->map_region(0x1000, 0x10000,
                                                MemoryProtection::ReadWrite);
        ASSERT_TRUE(map_result);

        m_cpu = std::make_unique<CpuState>();
        m_cpu->init_long_mode();
        m_cpu->gpr.set64(RSP, 0x10000);
        m_cpu->rip = 0x1000;
    }

    void write_code(const u8* code, usize len) {
        auto result = m_memory->write_bytes(0x1000, code, len);
        ASSERT_TRUE(result);
    }

    ExecResult execute_one() {
        Interpreter interp(*m_cpu, *m_memory);
        return interp.step();
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<CpuState> m_cpu;
};

// ============================================================================
// Decoder Tests
// ============================================================================

// ----------------------------------------------------------------------------
// CMPXCHG decode
// ----------------------------------------------------------------------------

TEST(AtomicsMiscDecode, CmpxchgEcxEax) {
    // 0F B1 C8 — CMPXCHG EAX, ECX  (r/m=EAX, r=ECX)
    auto inst = decode({0x0F, 0xB1, 0xC8});
    EXPECT_EQ(inst.opcode, Opcode::CMPXCHG_RM32_R32);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX (r/m)
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 1);  // ECX (reg)
}

TEST(AtomicsMiscDecode, CmpxchgRm8) {
    // 0F B0 CB — CMPXCHG BL, CL  (r/m=BL, r=CL)
    auto inst = decode({0x0F, 0xB0, 0xCB});
    EXPECT_EQ(inst.opcode, Opcode::CMPXCHG_RM8_R8);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[0].reg, 3);  // BL
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg8);
    EXPECT_EQ(inst.operands[1].reg, 1);  // CL
}

TEST(AtomicsMiscDecode, Cmpxchg64) {
    // 48 0F B1 C8 — CMPXCHG RAX, RCX
    auto inst = decode({0x48, 0x0F, 0xB1, 0xC8});
    EXPECT_EQ(inst.opcode, Opcode::CMPXCHG_RM64_R64);
}

TEST(AtomicsMiscDecode, CmpxchgWithLock) {
    // F0 0F B1 08 — LOCK CMPXCHG [EAX], ECX. The destination must be MEMORY:
    // Intel SDM Vol. 2, LOCK enumerates the instructions the prefix is legal
    // on "and only to those forms where the destination operand is a memory
    // operand".
    auto inst = decode({0xF0, 0x0F, 0xB1, 0x08});
    EXPECT_EQ(inst.opcode, Opcode::CMPXCHG_RM32_R32);
    EXPECT_TRUE(inst.prefixes.lock);
}

TEST(AtomicsMiscDecode, LockOnRegisterDestinationIsInvalid) {
    // F0 0F B1 C8 — LOCK CMPXCHG EAX, ECX. ModRM C8 is mod=11, so the
    // destination is a register and hardware raises #UD. This used to decode
    // happily; the assertion is the architectural behaviour, not the old
    // leniency.
    DecodedInstruction inst{};
    MemoryByteReader reader(nullptr, 0);
    const std::vector<u8> bytes{0xF0, 0x0F, 0xB1, 0xC8};
    MemoryByteReader r(bytes.data(), bytes.size());
    EXPECT_NE(decode_instruction(r, DecodeMode::Mode32, inst),
              DecodeError::Success);
}

// ----------------------------------------------------------------------------
// XADD decode
// ----------------------------------------------------------------------------

TEST(AtomicsMiscDecode, XaddEaxEcx) {
    // 0F C1 C8 — XADD EAX, ECX  (r/m=EAX, r=ECX)
    auto inst = decode({0x0F, 0xC1, 0xC8});
    EXPECT_EQ(inst.opcode, Opcode::XADD_RM32_R32);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 0);  // EAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 1);  // ECX
}

TEST(AtomicsMiscDecode, XaddRm8) {
    // 0F C0 CB — XADD BL, CL
    auto inst = decode({0x0F, 0xC0, 0xCB});
    EXPECT_EQ(inst.opcode, Opcode::XADD_RM8_R8);
}

TEST(AtomicsMiscDecode, Xadd64) {
    // 48 0F C1 C8 — XADD RAX, RCX
    auto inst = decode({0x48, 0x0F, 0xC1, 0xC8});
    EXPECT_EQ(inst.opcode, Opcode::XADD_RM64_R64);
}

// ----------------------------------------------------------------------------
// XCHG r/m, r decode
// ----------------------------------------------------------------------------

TEST(AtomicsMiscDecode, XchgRm32R32) {
    // 87 C1 — XCHG ECX, EAX  (r/m=ECX, r=EAX)
    auto inst = decode({0x87, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::XCHG_RM32_R32);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[0].reg, 1);  // ECX (r/m)
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 0);  // EAX (reg)
}

TEST(AtomicsMiscDecode, XchgRm8R8) {
    // 86 CB — XCHG BL, CL
    auto inst = decode({0x86, 0xCB});
    EXPECT_EQ(inst.opcode, Opcode::XCHG_RM8_R8);
}

TEST(AtomicsMiscDecode, XchgRm64R64) {
    // 48 87 C1 — XCHG RCX, RAX
    auto inst = decode({0x48, 0x87, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::XCHG_RM64_R64);
}

// ----------------------------------------------------------------------------
// MOVSXD decode
// ----------------------------------------------------------------------------

TEST(AtomicsMiscDecode, MovsxdRaxEcx) {
    // 48 63 C1 — MOVSXD RAX, ECX
    auto inst = decode({0x48, 0x63, 0xC1});
    EXPECT_EQ(inst.opcode, Opcode::MOVSXD_R64_RM32);
    EXPECT_EQ(inst.operand_count, 2);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg64);
    EXPECT_EQ(inst.operands[0].reg, 0);  // RAX
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg32);
    EXPECT_EQ(inst.operands[1].reg, 1);  // ECX
}

// ----------------------------------------------------------------------------
// Flag instruction decode
// ----------------------------------------------------------------------------

TEST(AtomicsMiscDecode, DecodeClc) {
    auto inst = decode({0xF8});
    EXPECT_EQ(inst.opcode, Opcode::CLC);
}

TEST(AtomicsMiscDecode, DecodeStc) {
    auto inst = decode({0xF9});
    EXPECT_EQ(inst.opcode, Opcode::STC);
}

TEST(AtomicsMiscDecode, DecodeCmc) {
    auto inst = decode({0xF5});
    EXPECT_EQ(inst.opcode, Opcode::CMC);
}

TEST(AtomicsMiscDecode, DecodeCld) {
    auto inst = decode({0xFC});
    EXPECT_EQ(inst.opcode, Opcode::CLD);
}

TEST(AtomicsMiscDecode, DecodeStd) {
    auto inst = decode({0xFD});
    EXPECT_EQ(inst.opcode, Opcode::STD);
}

TEST(AtomicsMiscDecode, DecodeLahf) {
    auto inst = decode({0x9F});
    EXPECT_EQ(inst.opcode, Opcode::LAHF);
}

TEST(AtomicsMiscDecode, DecodeSahf) {
    auto inst = decode({0x9E});
    EXPECT_EQ(inst.opcode, Opcode::SAHF);
}

// ============================================================================
// Execution Tests
// ============================================================================

// ----------------------------------------------------------------------------
// CMPXCHG exec — match (accumulator == r/m → reg → r/m)
// ----------------------------------------------------------------------------

TEST_F(AtomicsMiscTest, CmpxchgMatch) {
    // Setup: EAX = 0x42, ECX = 0x99
    // CMPXCHG EAX, ECX  → compare EAX(acc) with EAX(r/m) → equal
    //   so ECX → EAX(r/m), ZF=1
    // 0F B1 C8: r/m=EAX(0), reg=ECX(1)
    m_cpu->gpr.set32(RAX, 0x42);
    m_cpu->gpr.set32(RCX, 0x99);

    u8 code[] = { 0x0F, 0xB1, 0xC8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // Accumulator(EAX) == r/m(EAX) → ECX(0x99) stored into r/m(EAX)
    EXPECT_EQ(m_cpu->gpr.get32(RAX), 0x99U);
    EXPECT_EQ(m_cpu->gpr.get32(RCX), 0x99U);  // ECX unchanged
    EXPECT_TRUE(m_cpu->flags.zf());
}

TEST_F(AtomicsMiscTest, LockCmpxchgRm32MatchStores) {
    // Intel SDM Vol. 2 CMPXCHG + LOCK: EAX == [m32] → store r32, ZF=1.
    // F0 0F B1 16 = LOCK CMPXCHG [RSI], EDX
    ASSERT_TRUE(m_memory->write_u32(0x2000, 0).is_ok());
    m_cpu->gpr.set64(RSI, 0x2000);
    m_cpu->gpr.set32(RAX, 0);
    m_cpu->gpr.set32(RDX, 1);

    u8 code[] = {0xF0, 0x0F, 0xB1, 0x16};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.zf());
    auto r = m_memory->read_u32(0x2000);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 1u);
    EXPECT_EQ(m_cpu->gpr.get32(RAX), 0u);
}

TEST_F(AtomicsMiscTest, LockCmpxchgTwoThreadsExclusiveAcquire) {
    // TIMEPROC dispatch calls execute_instruction without lock_prefix_mutex.
    // Two host threads must not both observe ZF on LOCK CMPXCHG 0→1.
    constexpr GuestAddr kLock = 0x2000;
    ASSERT_TRUE(m_memory->write_u32(kLock, 0).is_ok());

    auto inst = decode({0xF0, 0x0F, 0xB1, 0x16});
    ASSERT_TRUE(inst.prefixes.lock);

    std::atomic<int> wins{0};
    auto worker = [&]() {
        CpuState cpu;
        cpu.init_long_mode();
        cpu.gpr.set64(RSI, kLock);
        cpu.gpr.set32(RAX, 0);
        cpu.gpr.set32(RDX, 1);
        Interpreter interp(cpu, *m_memory);
        auto& ctx = interp.context();
        ctx.instr_length = inst.length;
        ASSERT_EQ(execute_instruction(ctx, inst), ExecResult::Continue);
        if (cpu.flags.zf()) wins.fetch_add(1);
    };

    std::thread a(worker);
    std::thread b(worker);
    a.join();
    b.join();

    EXPECT_EQ(wins.load(), 1);
    auto r = m_memory->read_u32(kLock);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 1u);
}

// ----------------------------------------------------------------------------
// LOCK CMPXCHG16B m128 (Intel SDM Vol. 2 CMPXCHG16B + LOCK)
// Encoding: F0 48 0F C7 /1
// ----------------------------------------------------------------------------

TEST_F(AtomicsMiscTest, LockCmpxchg16bEqual) {
    // F0 48 0F C7 0E = LOCK CMPXCHG16B [RSI]
    // Intel SDM Vol. 2 CMPXCHG16B + LOCK: equal → ZF=1, store RCX:RBX.
    const GuestAddr m128 = 0x2000;
    const u64 mem_lo = 0x1111111111111111ull;
    const u64 mem_hi = 0x2222222222222222ull;
    const u64 rbx_v = 0x3333333333333333ull;
    const u64 rcx_v = 0x4444444444444444ull;

    m_cpu->gpr.set64(RSI, m128);
    m_cpu->gpr.set64(RAX, mem_lo);
    m_cpu->gpr.set64(RDX, mem_hi);
    m_cpu->gpr.set64(RBX, rbx_v);
    m_cpu->gpr.set64(RCX, rcx_v);

    u8 code[] = {0xF0, 0x48, 0x0F, 0xC7, 0x0E};
    write_code(code, sizeof(code));
    ASSERT_TRUE(m_memory->write_u64(m128, mem_lo));
    ASSERT_TRUE(m_memory->write_u64(m128 + 8, mem_hi));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    m_cpu->materialize_lazy_flags();

    EXPECT_TRUE(m_cpu->flags.zf());
    auto lo = m_memory->read_u64(m128);
    auto hi = m_memory->read_u64(m128 + 8);
    ASSERT_TRUE(lo.is_ok() && hi.is_ok());
    EXPECT_EQ(lo.value(), rbx_v);
    EXPECT_EQ(hi.value(), rcx_v);
    EXPECT_EQ(m_cpu->gpr.get64(RAX), mem_lo);
    EXPECT_EQ(m_cpu->gpr.get64(RDX), mem_hi);
}

TEST_F(AtomicsMiscTest, LockCmpxchg16bNotEqual) {
    // Intel SDM Vol. 2: mismatch → ZF=0, load m128 into RDX:RAX, memory unchanged.
    const GuestAddr m128 = 0x2000;
    const u64 mem_lo = 0xAAAAAAAAAAAAAAAAULL;
    const u64 mem_hi = 0xBBBBBBBBBBBBBBBBULL;

    m_cpu->gpr.set64(RSI, m128);
    m_cpu->gpr.set64(RAX, 1);
    m_cpu->gpr.set64(RDX, 2);
    m_cpu->gpr.set64(RBX, 3);
    m_cpu->gpr.set64(RCX, 4);

    u8 code[] = {0xF0, 0x48, 0x0F, 0xC7, 0x0E};
    write_code(code, sizeof(code));
    ASSERT_TRUE(m_memory->write_u64(m128, mem_lo));
    ASSERT_TRUE(m_memory->write_u64(m128 + 8, mem_hi));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    m_cpu->materialize_lazy_flags();

    EXPECT_FALSE(m_cpu->flags.zf());
    EXPECT_EQ(m_cpu->gpr.get64(RAX), mem_lo);
    EXPECT_EQ(m_cpu->gpr.get64(RDX), mem_hi);
    auto lo = m_memory->read_u64(m128);
    auto hi = m_memory->read_u64(m128 + 8);
    ASSERT_TRUE(lo.is_ok() && hi.is_ok());
    EXPECT_EQ(lo.value(), mem_lo);
    EXPECT_EQ(hi.value(), mem_hi);
}

TEST_F(AtomicsMiscTest, LockCmpxchg16bUnalignedRaisesGp) {
    // Intel SDM Vol. 2: CMPXCHG16B m128 not 16-byte aligned → #GP(0).
    m_cpu->gpr.set64(RSI, 0x2001);

    u8 code[] = {0xF0, 0x48, 0x0F, 0xC7, 0x0E};
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Exception);
}

// ----------------------------------------------------------------------------
// CMPXCHG exec — mismatch (accumulator != r/m → r/m → accumulator)
// ----------------------------------------------------------------------------

TEST_F(AtomicsMiscTest, CmpxchgMismatch) {
    // Setup: EAX = 0x42, ECX = 0x99, EDX = 0x10
    // CMPXCHG ECX, EDX → compare EAX(acc=0x42) with ECX(r/m=0x99) → not equal
    //   so ECX(r/m) → EAX(acc), ZF=0
    // 0F B1 D1: r/m=ECX(1), reg=EDX(2)
    m_cpu->gpr.set32(RAX, 0x42);
    m_cpu->gpr.set32(RCX, 0x99);
    m_cpu->gpr.set32(RDX, 0x10);

    u8 code[] = { 0x0F, 0xB1, 0xD1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    // Not equal → r/m(ECX=0x99) → accumulator(EAX)
    EXPECT_EQ(m_cpu->gpr.get32(RAX), 0x99U);
    EXPECT_EQ(m_cpu->gpr.get32(RCX), 0x99U);  // ECX unchanged
    EXPECT_EQ(m_cpu->gpr.get32(RDX), 0x10U);  // EDX unchanged
    EXPECT_FALSE(m_cpu->flags.zf());
}

// ----------------------------------------------------------------------------
// XADD exec
// ----------------------------------------------------------------------------

TEST_F(AtomicsMiscTest, XaddBasic) {
    // EAX = 10, ECX = 20
    // XADD EAX, ECX → r/m=EAX(0), reg=ECX(1)
    //   old_rm = 10, sum = 30
    //   ECX ← 10 (old r/m), EAX ← 30 (sum)
    // 0F C1 C8: r/m=EAX, reg=ECX
    m_cpu->gpr.set32(RAX, 10);
    m_cpu->gpr.set32(RCX, 20);

    u8 code[] = { 0x0F, 0xC1, 0xC8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(RAX), 30U);  // sum
    EXPECT_EQ(m_cpu->gpr.get32(RCX), 10U);  // old r/m value
    EXPECT_FALSE(m_cpu->flags.zf());         // 30 != 0
    EXPECT_FALSE(m_cpu->flags.cf());         // no carry
}

TEST_F(AtomicsMiscTest, XaddResultZero) {
    // EAX = 0, ECX = 0 → sum = 0
    m_cpu->gpr.set32(RAX, 0);
    m_cpu->gpr.set32(RCX, 0);

    u8 code[] = { 0x0F, 0xC1, 0xC8 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(RAX), 0U);
    EXPECT_TRUE(m_cpu->flags.zf());
}

// ----------------------------------------------------------------------------
// XCHG r/m, r exec
// ----------------------------------------------------------------------------

TEST_F(AtomicsMiscTest, XchgRmR) {
    // 87 C1 — XCHG ECX, EAX  (r/m=ECX, r=EAX)
    m_cpu->gpr.set32(RAX, 0xAAAA);
    m_cpu->gpr.set32(RCX, 0xBBBB);

    u8 code[] = { 0x87, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get32(RAX), 0xBBBBU);
    EXPECT_EQ(m_cpu->gpr.get32(RCX), 0xAAAAU);
}

// ----------------------------------------------------------------------------
// MOVSXD exec — sign-extension of negative value
// ----------------------------------------------------------------------------

TEST_F(AtomicsMiscTest, MovsxdNegative) {
    // 48 63 C1 — MOVSXD RAX, ECX
    // ECX = 0xFFFFFFFF (-1 as i32) → RAX should be 0xFFFFFFFFFFFFFFFF
    m_cpu->gpr.set32(RCX, 0xFFFFFFFF);
    m_cpu->gpr.set64(RAX, 0);

    u8 code[] = { 0x48, 0x63, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 0xFFFFFFFFFFFFFFFFULL);
}

TEST_F(AtomicsMiscTest, MovsxdPositive) {
    // 48 63 C1 — MOVSXD RAX, ECX
    // ECX = 0x7FFFFFFF → RAX should be 0x000000007FFFFFFF
    m_cpu->gpr.set32(RCX, 0x7FFFFFFF);
    m_cpu->gpr.set64(RAX, 0xDEAD);

    u8 code[] = { 0x48, 0x63, 0xC1 };
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 0x7FFFFFFFULL);
}

// ----------------------------------------------------------------------------
// CLC / STC / CMC
// ----------------------------------------------------------------------------

TEST_F(AtomicsMiscTest, ClcClearsCF) {
    m_cpu->flags.set_cf(true);
    u8 code[] = { 0xF8 };  // CLC
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FALSE(m_cpu->flags.cf());
}

TEST_F(AtomicsMiscTest, StcSetsCF) {
    m_cpu->flags.set_cf(false);
    u8 code[] = { 0xF9 };  // STC
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.cf());
}

TEST_F(AtomicsMiscTest, CmcComplementsCF) {
    m_cpu->flags.set_cf(false);
    u8 code[] = { 0xF5 };  // CMC
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.cf());
}

TEST_F(AtomicsMiscTest, CmcComplementsCFAgain) {
    m_cpu->flags.set_cf(true);
    u8 code[] = { 0xF5 };  // CMC
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FALSE(m_cpu->flags.cf());
}

// CLC/STC write only CF. A pending ALU FlagOp stays live so ZF/SF/OF/PF
// can still be answered lazily; CF is recorded in cf_overlay so a later
// materialize cannot undo the write.
TEST_F(AtomicsMiscTest, StcPreservesPendingLazyFlags) {
    // Pending CMP 3,5 borrows (CF=1); STC must leave CF set regardless.
    m_cpu->flag_op = CpuState::FlagOp::Sub32;
    m_cpu->flag_src = 5;
    m_cpu->flag_dst = static_cast<u32>(3u - 5u);

    u8 code[] = { 0xF9 };  // STC
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.cf());
    EXPECT_EQ(m_cpu->flag_op, CpuState::FlagOp::Sub32);
    EXPECT_EQ(m_cpu->cf_overlay, 2);
    m_cpu->materialize_lazy_flags();
    EXPECT_TRUE(m_cpu->flags.cf());
    EXPECT_EQ(m_cpu->flag_op, CpuState::FlagOp::None);
    EXPECT_EQ(m_cpu->cf_overlay, 0);
}

TEST_F(AtomicsMiscTest, ClcPreservesPendingLazyFlags) {
    // Pending CMP 3,5 borrows (CF=1); CLC must still clear CF.
    m_cpu->flag_op = CpuState::FlagOp::Sub32;
    m_cpu->flag_src = 5;
    m_cpu->flag_dst = static_cast<u32>(3u - 5u);

    u8 code[] = { 0xF8 };  // CLC
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_EQ(m_cpu->flag_op, CpuState::FlagOp::Sub32);
    EXPECT_EQ(m_cpu->cf_overlay, 1);
    m_cpu->materialize_lazy_flags();
    EXPECT_FALSE(m_cpu->flags.cf());
    EXPECT_EQ(m_cpu->flag_op, CpuState::FlagOp::None);
    EXPECT_EQ(m_cpu->cf_overlay, 0);
}

TEST_F(AtomicsMiscTest, LahfObservesPendingLazyFlags) {
    // Pending CMP 7,7 is equal, so AH must report ZF=1 even though the stale
    // EFLAGS image says otherwise.
    m_cpu->flags.rflags = 0;
    m_cpu->gpr.rax = 0;
    m_cpu->flag_op = CpuState::FlagOp::Sub32;
    m_cpu->flag_src = 7;
    m_cpu->flag_dst = 0;

    u8 code[] = { 0x9F };  // LAHF
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ((m_cpu->gpr.rax >> 8) & 0x40, 0x40u);  // ZF
}

// ----------------------------------------------------------------------------
// CLD / STD
// ----------------------------------------------------------------------------

TEST_F(AtomicsMiscTest, CldClearsDF) {
    m_cpu->flags.set_df(true);
    u8 code[] = { 0xFC };  // CLD
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FALSE(m_cpu->flags.df());
}

TEST_F(AtomicsMiscTest, StdSetsDF) {
    m_cpu->flags.set_df(false);
    u8 code[] = { 0xFD };  // STD
    write_code(code, sizeof(code));
    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.df());
}

// ----------------------------------------------------------------------------
// LAHF — flags → AH
// ----------------------------------------------------------------------------

TEST_F(AtomicsMiscTest, LahfLoadsFlags) {
    // Set specific flags: SF=1, ZF=1, CF=1 — low byte of rflags
    m_cpu->flags.set_sf(true);   // bit 7
    m_cpu->flags.set_zf(true);   // bit 6
    m_cpu->flags.set_af(false);  // bit 4
    m_cpu->flags.set_pf(true);   // bit 2
    m_cpu->flags.set_cf(true);   // bit 0
    // Bit 1 is always set → low byte should be 0xC7
    m_cpu->gpr.rax = 0;

    u8 code[] = { 0x9F };  // LAHF
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    u8 ah = static_cast<u8>((m_cpu->gpr.rax >> 8) & 0xFF);
    EXPECT_EQ(ah, static_cast<u8>(m_cpu->flags.rflags & 0xFF));
    // Check specific bits: SF(7)=1, ZF(6)=1, AF(4)=0, PF(2)=1, CF(0)=1, bit1=1
    // = 1100_0111 = 0xC7
    EXPECT_EQ(ah, 0xC7);
}

// ----------------------------------------------------------------------------
// SAHF — AH → flags
// ----------------------------------------------------------------------------

TEST_F(AtomicsMiscTest, SahfSetsFlags) {
    // AH = 0xD5 → SF=1, ZF=1, AF=1, PF=1, CF=1
    // 0xD5 = 1101_0101 → bit7=SF, bit6=ZF, bit4=AF, bit2=PF, bit0=CF all set
    m_cpu->gpr.rax = static_cast<u64>(0xD5) << 8;
    m_cpu->flags = Flags{};  // Reset flags

    u8 code[] = { 0x9E };  // SAHF
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_TRUE(m_cpu->flags.sf());
    EXPECT_TRUE(m_cpu->flags.zf());
    EXPECT_TRUE(m_cpu->flags.af());
    EXPECT_TRUE(m_cpu->flags.pf());
    EXPECT_TRUE(m_cpu->flags.cf());
}

TEST_F(AtomicsMiscTest, SahfClearsFlags) {
    // AH = 0x02 → all SAHF-controlled flags clear (bit1 always set)
    m_cpu->gpr.rax = static_cast<u64>(0x02) << 8;
    m_cpu->flags.set_sf(true);
    m_cpu->flags.set_zf(true);
    m_cpu->flags.set_af(true);
    m_cpu->flags.set_pf(true);
    m_cpu->flags.set_cf(true);

    u8 code[] = { 0x9E };  // SAHF
    write_code(code, sizeof(code));

    EXPECT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_FALSE(m_cpu->flags.sf());
    EXPECT_FALSE(m_cpu->flags.zf());
    EXPECT_FALSE(m_cpu->flags.af());
    EXPECT_FALSE(m_cpu->flags.pf());
    EXPECT_FALSE(m_cpu->flags.cf());
}

// ----------------------------------------------------------------------------
// LAHF + SAHF round-trip
// ----------------------------------------------------------------------------

TEST_F(AtomicsMiscTest, LahfSahfRoundTrip) {
    // Set known flags, LAHF, clear flags, SAHF, verify restored
    m_cpu->flags.set_sf(true);
    m_cpu->flags.set_zf(false);
    m_cpu->flags.set_af(true);
    m_cpu->flags.set_pf(false);
    m_cpu->flags.set_cf(true);
    u64 original_low = m_cpu->flags.rflags & 0xD5;  // SAHF-relevant bits

    m_cpu->gpr.rax = 0;

    // LAHF + SAHF sequence
    u8 code[] = { 0x9F, 0x9E };  // LAHF; SAHF
    write_code(code, sizeof(code));

    Interpreter interp(*m_cpu, *m_memory);

    // Execute LAHF
    EXPECT_EQ(interp.step(), ExecResult::Continue);
    u8 ah = static_cast<u8>((m_cpu->gpr.rax >> 8) & 0xFF);

    // Clear the flags
    m_cpu->flags.set_sf(false);
    m_cpu->flags.set_zf(true);
    m_cpu->flags.set_af(false);
    m_cpu->flags.set_pf(true);
    m_cpu->flags.set_cf(false);

    // Execute SAHF — should restore from AH
    EXPECT_EQ(interp.step(), ExecResult::Continue);
    u64 restored_low = m_cpu->flags.rflags & 0xD5;
    EXPECT_EQ(restored_low, original_low);
}

namespace {

BasicBlock decode_lahf_sahf_block(const u8* code, usize len) {
    BasicBlock block;
    block.start_addr = 0x1000;
    block.is_64bit = true;
    MemoryByteReader reader(code, len);
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

void expect_lahf_sahf_jit_matches_interpreter(GuestMemory& memory,
                                              const CpuState& start,
                                              const std::vector<u8>& code) {
    for (usize i = 0; i < code.size(); ++i) {
        ASSERT_TRUE(memory.write_u8(0x1000 + i, code[i]).is_ok());
    }

    BasicBlock block = decode_lahf_sahf_block(code.data(), code.size());
    ASSERT_GT(block.instruction_count, 0u);

    CpuState icpu = start;
    icpu.rip = 0x1000;
    {
        Interpreter interp(icpu, memory);
        for (u32 i = 0; i < block.instruction_count; ++i) {
            EXPECT_EQ(interp.step(), ExecResult::Continue);
        }
    }
    icpu.materialize_lazy_flags();

    CpuState jcpu = start;
    jcpu.rip = 0x1000;
    JitCompiler jit(memory);
    CompiledBlock* cb = jit.compile(block);
    ASSERT_NE(cb, nullptr);
    EXPECT_EQ(jit.execute(*cb, jcpu), JitExecResult::Continue);
    jcpu.materialize_lazy_flags();

    EXPECT_EQ(icpu.gpr.rax, jcpu.gpr.rax);
    EXPECT_EQ(icpu.flags.rflags, jcpu.flags.rflags);
}

}  // namespace

TEST_F(AtomicsMiscTest, LahfJitMatchesInterpreter) {
    m_cpu->flags.set_sf(true);
    m_cpu->flags.set_zf(true);
    m_cpu->flags.set_af(false);
    m_cpu->flags.set_pf(true);
    m_cpu->flags.set_cf(true);
    m_cpu->gpr.rax = 0x1111222233330000ull;
    expect_lahf_sahf_jit_matches_interpreter(*m_memory, *m_cpu, {0x9F});
}

TEST_F(AtomicsMiscTest, SahfJitMatchesInterpreter) {
    m_cpu->gpr.rax = 0xAAAABBBBCCCCD500ull;
    m_cpu->flags = Flags{};
    expect_lahf_sahf_jit_matches_interpreter(*m_memory, *m_cpu, {0x9E});
}

// ----------------------------------------------------------------------------
// 66 90+r is XCHG AX, r16 -- a SIXTEEN-bit exchange
// ----------------------------------------------------------------------------
//
// The operand-size prefix was ignored here, so `66 91` decoded as XCHG EAX,
// ECX. That is not a missing form but a wrong RESULT, and a quiet one: the
// low sixteen bits it produces are exactly right, and only the upper halves --
// which a 16-bit exchange must leave untouched -- are destroyed.
TEST_F(AtomicsMiscTest, XchgAxR16IsSixteenBitsNotThirtyTwo) {
    const auto inst = decode({0x66, 0x91});
    EXPECT_EQ(inst.opcode, Opcode::XCHG_AX_R16);
    ASSERT_EQ(inst.operand_count, 2u);
    EXPECT_EQ(inst.operands[0].type, OperandType::Reg16);
    EXPECT_EQ(inst.operands[1].type, OperandType::Reg16);
    EXPECT_EQ(inst.operands[0].reg, RAX);
    EXPECT_EQ(inst.operands[1].reg, RCX);

    const u8 code[] = {0x66, 0x91};   // xchg ax, cx
    write_code(code, sizeof(code));
    m_cpu->gpr.set64(RAX, 0xAAAAAAAAAAAA1111ull);
    m_cpu->gpr.set64(RCX, 0xCCCCCCCCCCCC2222ull);
    ASSERT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 0xAAAAAAAAAAAA2222ull);
    EXPECT_EQ(m_cpu->gpr.get64(RCX), 0xCCCCCCCCCCCC1111ull);

    // Without the prefix it is still the 32-bit form, which zero-extends.
    const u8 wide[] = {0x91};
    m_cpu->rip = 0x1000;
    write_code(wide, sizeof(wide));
    m_cpu->gpr.set64(RAX, 0xAAAAAAAAAAAA1111ull);
    m_cpu->gpr.set64(RCX, 0xCCCCCCCCCCCC2222ull);
    ASSERT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 0xCCCC2222ull);
    EXPECT_EQ(m_cpu->gpr.get64(RCX), 0xAAAA1111ull);

    // REX.W wins over the prefix: 66 48 91 is the 64-bit exchange.
    const u8 quad[] = {0x66, 0x48, 0x91};
    m_cpu->rip = 0x1000;
    write_code(quad, sizeof(quad));
    m_cpu->gpr.set64(RAX, 0xAAAAAAAAAAAA1111ull);
    m_cpu->gpr.set64(RCX, 0xCCCCCCCCCCCC2222ull);
    ASSERT_EQ(execute_one(), ExecResult::Continue);
    EXPECT_EQ(m_cpu->gpr.get64(RAX), 0xCCCCCCCCCCCC2222ull);
    EXPECT_EQ(m_cpu->gpr.get64(RCX), 0xAAAAAAAAAAAA1111ull);
}

// ----------------------------------------------------------------------------
// CMPXCHG's comparison is `accumulator - DEST`, measured on silicon
// ----------------------------------------------------------------------------
//
// Every value below was read off a real Alder Lake i9-12900KF running
// `tools/oracle_probe/cmpxchg_flag_probe.c`, whose output
// is recorded beside it. Vitreo had the subtraction the other way round -- in
// the interpreter, in three stencil arms, in the 64-bit arm and in every lock
// helper -- because `HardwareDifferential` treats Rosetta 2 as an x86 core and
// Rosetta computes `DEST - accumulator`. Both probes are in tools/oracle_probe
// with their outputs; they disagree, and the silicon is right (Intel SDM
// Vol. 2 CMPXCHG: "compares the value in the AL/AX/EAX/RAX register WITH the
// first operand").
//
// ZF is identical under both readings. That is the whole reason this survived:
// a lock loop reads only ZF, so every spinlock kept working while a CMPXCHG
// followed by JB/JL/JS branched the wrong way.
TEST_F(AtomicsMiscTest, CmpxchgFlagsAreAccumulatorMinusDestination) {
    struct Case {
        unsigned width; u64 acc; u64 dst; u64 src;
        u64 want_dst; u64 want_acc; u64 want_flags;
    };
    // width, acc, dst, src, -> dst, acc, flags (CF|PF|AF|ZF|SF|OF only)
    static const Case kCases[] = {
        { 8, 0x01, 0x02, 0x55, 0x02, 0x02, 0x0095},
        { 8, 0x02, 0x01, 0x55, 0x01, 0x01, 0x0000},
        { 8, 0x80, 0x01, 0x55, 0x01, 0x01, 0x0810},
        { 8, 0x01, 0x80, 0x55, 0x80, 0x80, 0x0885},
        { 8, 0x00, 0xFF, 0x55, 0xFF, 0xFF, 0x0011},
        { 8, 0xFF, 0x00, 0x55, 0x00, 0x00, 0x0084},
        { 8, 0x08, 0x09, 0x55, 0x09, 0x09, 0x0095},
        { 8, 0x42, 0x42, 0x55, 0x55, 0x42, 0x0044},
        { 8, 0x00, 0x00, 0x55, 0x55, 0x00, 0x0044},
        {16, 0x0001, 0x0002, 0x5555, 0x0002, 0x0002, 0x0095},
        {16, 0x0002, 0x0001, 0x5555, 0x0001, 0x0001, 0x0000},
        {16, 0x8000, 0x0001, 0x5555, 0x0001, 0x0001, 0x0814},
        {16, 0x4242, 0x4242, 0x5555, 0x5555, 0x4242, 0x0044},
        {32, 0x00000001, 0x00000002, 0x55555555, 0x00000002, 0x00000002, 0x0095},
        {32, 0x00000002, 0x00000001, 0x55555555, 0x00000001, 0x00000001, 0x0000},
        {32, 0x80000000, 0x00000001, 0x55555555, 0x00000001, 0x00000001, 0x0814},
        {32, 0x42424242, 0x42424242, 0x55555555, 0x55555555, 0x42424242, 0x0044},
    };
    constexpr u64 kArith = 0x8D5ull;   // CF PF AF ZF SF OF
    for (const Case& c : kCases) {
        SCOPED_TRACE(testing::Message()
                     << "w" << c.width << " acc=" << std::hex << c.acc
                     << " dst=" << c.dst);
        // cmpxchg bl,cl / bx,cx / ebx,ecx -- accumulator is AL/AX/EAX.
        std::vector<u8> code;
        if (c.width == 16) code.push_back(0x66);
        code.push_back(0x0F);
        code.push_back(c.width == 8 ? 0xB0 : 0xB1);
        code.push_back(0xCB);
        m_cpu->rip = 0x1000;
        write_code(code.data(), code.size());
        // Entry flags with every status bit but OF already set, so a flag the
        // instruction leaves alone is distinguishable from one it clears.
        m_cpu->flags.rflags = 0x2D5;
        m_cpu->gpr.set64(RAX, 0xAAAAAAAAAAAAAA00ull | c.acc);
        m_cpu->gpr.set64(RBX, 0xBBBBBBBBBBBBBB00ull | c.dst);
        m_cpu->gpr.set64(RCX, 0xCCCCCCCCCCCCCC00ull | c.src);
        if (c.width == 8) {
            m_cpu->gpr.set8l(RAX, static_cast<u8>(c.acc));
            m_cpu->gpr.set8l(RBX, static_cast<u8>(c.dst));
            m_cpu->gpr.set8l(RCX, static_cast<u8>(c.src));
        } else if (c.width == 16) {
            m_cpu->gpr.set16(RAX, static_cast<u16>(c.acc));
            m_cpu->gpr.set16(RBX, static_cast<u16>(c.dst));
            m_cpu->gpr.set16(RCX, static_cast<u16>(c.src));
        } else {
            m_cpu->gpr.set32(RAX, static_cast<u32>(c.acc));
            m_cpu->gpr.set32(RBX, static_cast<u32>(c.dst));
            m_cpu->gpr.set32(RCX, static_cast<u32>(c.src));
        }
        ASSERT_EQ(execute_one(), ExecResult::Continue);
        m_cpu->materialize_lazy_flags();
        const u64 mask = c.width == 8 ? 0xFFull
                       : c.width == 16 ? 0xFFFFull : 0xFFFFFFFFull;
        EXPECT_EQ(m_cpu->gpr.get64(RBX) & mask, c.want_dst) << "destination";
        EXPECT_EQ(m_cpu->gpr.get64(RAX) & mask, c.want_acc) << "accumulator";
        EXPECT_EQ(m_cpu->flags.rflags & kArith, c.want_flags) << "flags";
    }
}
