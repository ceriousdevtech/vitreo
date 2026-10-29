// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Inline guest-memory fast-path tests for long mode (PE32+ guests).
//
// In 64-bit blocks the JIT accesses guest memory inline when the address falls
// inside one of the full-width JitContext region slots: x64_region/x64_region2
// for loads, x64_wregion for stores. A hit skips the C helper entirely, and
// with it the permission check, the SMC notification and the access-violation
// raise -- which is only sound because a store region is published solely when
// it is writable, non-executable and free of compiled code.
//
// These tests cover the cases where that invariant is load-bearing, all at
// addresses above 4 GB (the reason long mode could not use the legacy 32-bit
// slots): region edges, read-only and code-bearing regions, and truncation of
// a 64-bit address to its low 32 bits. A regression here is silent memory
// corruption rather than a crash, so memory is compared in full against the
// interpreter.

#include <gtest/gtest.h>
#include <cstring>
#include <functional>
#include <vector>
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/memory/write_watch_traps.hpp"
#include "core/base/perf_window.hpp"
#include <vitreo/diagnostics.hpp>

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

constexpr u8 RAX = static_cast<u8>(RegIndex::RAX);
constexpr u8 RBX = static_cast<u8>(RegIndex::RBX);
constexpr u8 RSP = static_cast<u8>(RegIndex::RSP);

// Guest layout, all above 4 GB. kDataA's low 32 bits deliberately coincide
// with kShadow's address: an access that truncates a 64-bit address lands in
// kShadow, which is snapshotted and compared like every other region.
constexpr GuestAddr kCodeBase = 0x140000000ull;   // typical PE32+ image base
constexpr usize     kCodeSize = 0x4000;
constexpr GuestAddr kDataA    = 0x240000000ull;
constexpr usize     kDataSize = 0x2000;
constexpr GuestAddr kDataB    = 0x340000000ull;
constexpr GuestAddr kRoBase   = 0x440000000ull;
constexpr usize     kRoSize   = 0x1000;
constexpr GuestAddr kStack    = 0x540000000ull;
constexpr usize     kStackSize = 0x4000;
constexpr GuestAddr kShadow   = 0x40000000ull;    // low 32 bits of kDataA
constexpr usize     kShadowSize = 0x2000;

class JitX64InlineMemTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(kCodeBase, kCodeSize,
                                         MemoryProtection::ReadWriteExecute));
        ASSERT_TRUE(m_memory->map_region(kDataA, kDataSize,
                                         MemoryProtection::ReadWrite));
        ASSERT_TRUE(m_memory->map_region(kDataB, kDataSize,
                                         MemoryProtection::ReadWrite));
        ASSERT_TRUE(m_memory->map_region(kRoBase, kRoSize,
                                         MemoryProtection::Read));
        ASSERT_TRUE(m_memory->map_region(kStack, kStackSize,
                                         MemoryProtection::ReadWrite));
        ASSERT_TRUE(m_memory->map_region(kShadow, kShadowSize,
                                         MemoryProtection::ReadWrite));
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_cache = std::make_unique<BlockCache>();
        m_jit->set_block_cache(m_cache.get());
    }

    BasicBlock build_block(const std::vector<u8>& code, GuestAddr at) {
        BasicBlock block;
        block.start_addr = at;
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
        cpu.gpr.set64(RSP, kStack + kStackSize - 0x100);
        cpu.rip = kCodeBase;
    }

    // Every byte of every writable region plus the read-only region and the
    // low-address shadow, so a store landing at a wrong offset -- or at a
    // truncated address -- is caught wherever it lands.
    std::vector<u8> snapshot_data() {
        std::vector<u8> out(kDataSize * 2 + kRoSize + kShadowSize);
        usize at = 0;
        EXPECT_TRUE(m_memory->read_bytes(kDataA, out.data() + at, kDataSize));
        at += kDataSize;
        EXPECT_TRUE(m_memory->read_bytes(kDataB, out.data() + at, kDataSize));
        at += kDataSize;
        EXPECT_TRUE(m_memory->read_bytes(kRoBase, out.data() + at, kRoSize));
        at += kRoSize;
        EXPECT_TRUE(m_memory->read_bytes(kShadow, out.data() + at,
                                         kShadowSize));
        return out;
    }

    void restore_data(const std::vector<u8>& snap) {
        EXPECT_TRUE(m_memory->write_bytes(kDataA, snap.data(), kDataSize));
        EXPECT_TRUE(m_memory->write_bytes(kDataB, snap.data() + kDataSize,
                                          kDataSize));
        EXPECT_TRUE(m_memory->write_bytes(
            kShadow, snap.data() + kDataSize * 2 + kRoSize, kShadowSize));
    }

    /// Run identical code through interpreter and JIT; require identical
    /// registers, memory and access-violation outcome.
    void check_parity(const std::vector<u8>& code,
                     std::function<void(CpuState&)> setup = {}) {
        ASSERT_TRUE(m_memory->write_bytes(kCodeBase, code.data(), code.size()));
        BasicBlock block = build_block(code, kCodeBase);
        ASSERT_GT(block.instruction_count, 0u);

        const std::vector<u8> initial = snapshot_data();

        // ---- Interpreter (semantic reference) ----
        CpuState icpu;
        init_cpu(icpu);
        if (setup) setup(icpu);
        ExecResult iresult;
        {
            Interpreter interp(icpu, *m_memory);
            iresult = interp.run(block.instruction_count);
        }
        icpu.materialize_lazy_flags();
        const std::vector<u8> interp_mem = snapshot_data();
        // The engines report a bad access differently: the interpreter returns
        // ExecResult::Exception, the JIT sets cpu.pending_raise. Compare the
        // fault/no-fault outcome, not the mechanism.
        const bool interp_raised = (iresult == ExecResult::Exception)
                                   || icpu.pending_raise.active;

        restore_data(initial);

        // ---- JIT ----
        CpuState jcpu;
        init_cpu(jcpu);
        if (setup) setup(jcpu);
        CompiledBlock* cb = m_jit->compile(block);
        ASSERT_NE(cb, nullptr) << "JIT compilation failed";
        (void)m_jit->execute(*cb, jcpu);
        jcpu.materialize_lazy_flags();
        const std::vector<u8> jit_mem = snapshot_data();
        const bool jit_raised = jcpu.pending_raise.active;

        EXPECT_EQ(interp_raised, jit_raised)
            << "access-violation outcome diverged: interp=" << interp_raised
            << " jit=" << jit_raised;

        for (u8 i = 0; i < 16; ++i) {
            EXPECT_EQ(icpu.gpr.get64(i), jcpu.gpr.get64(i))
                << "GPR " << static_cast<unsigned>(i) << " diverged";
        }

        ASSERT_EQ(interp_mem.size(), jit_mem.size());
        for (usize i = 0; i < interp_mem.size(); ++i) {
            if (interp_mem[i] == jit_mem[i]) continue;
            usize off = i;
            GuestAddr where;
            if (off < kDataSize) {
                where = kDataA + off;
            } else if ((off -= kDataSize) < kDataSize) {
                where = kDataB + off;
            } else if ((off -= kDataSize) < kRoSize) {
                where = kRoBase + off;
            } else {
                where = kShadow + (off - kRoSize);
            }
            ADD_FAILURE() << "memory diverged at 0x" << std::hex << where
                          << ": interp=0x"
                          << static_cast<unsigned>(interp_mem[i]) << " jit=0x"
                          << static_cast<unsigned>(jit_mem[i]);
            break;  // one report is enough; the whole region is suspect
        }

        restore_data(initial);
    }

    // Stores/loads through RBX, which the setup lambda points at the target.
    static std::vector<u8> store64_rbx() { return {0x48, 0x89, 0x03}; }
    static std::vector<u8> store32_rbx() { return {0x89, 0x03}; }
    static std::vector<u8> store16_rbx() { return {0x66, 0x89, 0x03}; }
    static std::vector<u8> store8_rbx()  { return {0x88, 0x03}; }
    static std::vector<u8> load64_rbx()  { return {0x48, 0x8B, 0x03}; }
    static std::vector<u8> load32_rbx()  { return {0x8B, 0x03}; }
    static std::vector<u8> load8_rbx()   { return {0x8A, 0x03}; }

    static std::function<void(CpuState&)> at(GuestAddr addr, u64 value) {
        return [addr, value](CpuState& c) {
            c.gpr.set64(RBX, addr);
            c.gpr.set64(RAX, value);
        };
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
    std::unique_ptr<BlockCache> m_cache;
};

// ---------------------------------------------------------------------------
// Parity above 4 GB: the inline path must produce what the helper produces.
// ---------------------------------------------------------------------------

TEST_F(JitX64InlineMemTest, Store64HighAddressMatchesInterpreter) {
    check_parity(store64_rbx(), at(kDataA + 0x40, 0x0123456789ABCDEFull));
}

TEST_F(JitX64InlineMemTest, Store32HighAddressMatchesInterpreter) {
    check_parity(store32_rbx(), at(kDataA + 0x40, 0xDEADBEEFull));
}

TEST_F(JitX64InlineMemTest, Store16HighAddressMatchesInterpreter) {
    check_parity(store16_rbx(), at(kDataA + 0x40, 0xDEADBEEFull));
}

TEST_F(JitX64InlineMemTest, Store8HighAddressMatchesInterpreter) {
    check_parity(store8_rbx(), at(kDataA + 0x40, 0xDEADBEEFull));
}

TEST_F(JitX64InlineMemTest, Load64HighAddressMatchesInterpreter) {
    ASSERT_TRUE(m_memory->write_u64(kDataA + 0x80, 0xFEDCBA9876543210ull));
    check_parity(load64_rbx(), at(kDataA + 0x80, 0));
}

TEST_F(JitX64InlineMemTest, Load32HighAddressMatchesInterpreter) {
    ASSERT_TRUE(m_memory->write_u64(kDataA + 0x80, 0xFEDCBA9876543210ull));
    check_parity(load32_rbx(), at(kDataA + 0x80, 0xFFFFFFFFFFFFFFFFull));
}

TEST_F(JitX64InlineMemTest, Load8HighAddressMatchesInterpreter) {
    ASSERT_TRUE(m_memory->write_u64(kDataA + 0x80, 0xFEDCBA9876543210ull));
    check_parity(load8_rbx(), at(kDataA + 0x80, 0xFFFFFFFFFFFFFF00ull));
}

// A narrow store must touch exactly its own bytes: catches a mis-sized STR,
// which the correct byte at the target address alone would not reveal.
TEST_F(JitX64InlineMemTest, Store8LeavesNeighboursIntact) {
    ASSERT_TRUE(m_memory->write_u64(kDataA + 0x40, 0xAAAAAAAAAAAAAAAAull));
    check_parity(store8_rbx(), at(kDataA + 0x41, 0x11223344ull));
}

TEST_F(JitX64InlineMemTest, Store16LeavesNeighboursIntact) {
    ASSERT_TRUE(m_memory->write_u64(kDataA + 0x40, 0xAAAAAAAAAAAAAAAAull));
    check_parity(store16_rbx(), at(kDataA + 0x41, 0x11223344ull));
}

TEST_F(JitX64InlineMemTest, Store32LeavesNeighboursIntact) {
    ASSERT_TRUE(m_memory->write_u64(kDataA + 0x40, 0xAAAAAAAAAAAAAAAAull));
    check_parity(store32_rbx(), at(kDataA + 0x41, 0x11223344ull));
}

// ---------------------------------------------------------------------------
// A 64-bit address must not be truncated to its low 32 bits. kShadow is mapped
// at exactly kDataA's low half, so a truncating access is visible as a write
// to the wrong region (or a load of the wrong value) rather than as a fault.
// ---------------------------------------------------------------------------

TEST_F(JitX64InlineMemTest, HighStoreDoesNotAliasLowShadowRegion) {
    ASSERT_TRUE(m_memory->write_u64(kShadow + 0x40, 0x5555555555555555ull));
    check_parity(store64_rbx(), at(kDataA + 0x40, 0x0123456789ABCDEFull));

    u64 shadow = 0;
    ASSERT_TRUE(m_memory->read_bytes(kShadow + 0x40,
                                     reinterpret_cast<u8*>(&shadow), 8));
    EXPECT_EQ(shadow, 0x5555555555555555ull)
        << "a store to a >4GB address was truncated onto its low 32 bits";
}

TEST_F(JitX64InlineMemTest, HighLoadDoesNotAliasLowShadowRegion) {
    ASSERT_TRUE(m_memory->write_u64(kDataA + 0x40, 0x0123456789ABCDEFull));
    ASSERT_TRUE(m_memory->write_u64(kShadow + 0x40, 0x5555555555555555ull));
    check_parity(load64_rbx(), at(kDataA + 0x40, 0));
}

// Stores into the shadow region right after a high-address store: the slot
// holds kDataA, so this must miss and republish rather than write at
// kDataA + (low bits).
TEST_F(JitX64InlineMemTest, MixedHighAndLowStoresAllLand) {
    std::vector<u8> code;
    // MOV [RBX], EAX ; MOV RBX, kShadow+0x40 ; MOV [RBX], EAX
    for (u8 b : store32_rbx()) code.push_back(b);
    code.push_back(0x48);  // MOV RBX, imm64
    code.push_back(0xBB);
    const u64 low = kShadow + 0x40;
    for (int i = 0; i < 8; ++i) {
        code.push_back(static_cast<u8>((low >> (i * 8)) & 0xFF));
    }
    for (u8 b : store32_rbx()) code.push_back(b);
    check_parity(code, at(kDataA + 0x40, 0x0BADF00Dull));
}

// ---------------------------------------------------------------------------
// Region edges. The bounds test uses the *last* byte touched; without it a
// wide access at the tail of a region runs off the end of the host mapping.
// ---------------------------------------------------------------------------

TEST_F(JitX64InlineMemTest, Store64AtLastValidOffsetStaysInBounds) {
    check_parity(store64_rbx(),
                 at(kDataA + kDataSize - 8, 0x0123456789ABCDEFull));
}

TEST_F(JitX64InlineMemTest, Store64StraddlingRegionEndDoesNotCorrupt) {
    check_parity(store64_rbx(),
                 at(kDataA + kDataSize - 4, 0x0123456789ABCDEFull));
}

TEST_F(JitX64InlineMemTest, Store32StraddlingRegionEndDoesNotCorrupt) {
    check_parity(store32_rbx(), at(kDataA + kDataSize - 2, 0xCAFEBABEull));
}

TEST_F(JitX64InlineMemTest, Store8AtLastValidOffsetStaysInBounds) {
    check_parity(store8_rbx(), at(kDataA + kDataSize - 1, 0x5Aull));
}

TEST_F(JitX64InlineMemTest, Load64StraddlingRegionEndRaises) {
    check_parity(load64_rbx(), at(kDataA + kDataSize - 4, 0));
}

TEST_F(JitX64InlineMemTest, Load8AtLastValidOffsetStaysInBounds) {
    check_parity(load8_rbx(), at(kDataA + kDataSize - 1, 0));
}

TEST_F(JitX64InlineMemTest, StoreToUnmappedHighAddressRaises) {
    check_parity(store64_rbx(), at(0x7F0000000000ull, 0xDEADBEEFull));
}

TEST_F(JitX64InlineMemTest, LoadFromUnmappedHighAddressRaises) {
    check_parity(load64_rbx(), at(0x7F0000000000ull, 0));
}

// ---------------------------------------------------------------------------
// Permission. The inline store performs no permission check, so a read-only
// region must never be published.
// ---------------------------------------------------------------------------

TEST_F(JitX64InlineMemTest, Store64ToReadOnlyRegionRaisesAndDoesNotWrite) {
    check_parity(store64_rbx(), at(kRoBase + 0x10, 0x0123456789ABCDEFull));
}

TEST_F(JitX64InlineMemTest, Store8ToReadOnlyRegionRaisesAndDoesNotWrite) {
    check_parity(store8_rbx(), at(kRoBase + 0x10, 0x5Aull));
}

// A region that becomes read-only after publication must stop being eligible:
// protect_region publishes a ranged shootdown that retracts the slot.
TEST_F(JitX64InlineMemTest, ProtectToReadOnlyRetractsPublishedRegion) {
    const auto code = store32_rbx();
    ASSERT_TRUE(m_memory->write_bytes(kCodeBase, code.data(), code.size()));
    BasicBlock block = build_block(code, kCodeBase);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);

    {
        CpuState cpu;
        init_cpu(cpu);
        cpu.gpr.set64(RBX, kDataA + 0x40);
        cpu.gpr.set64(RAX, 0x11111111);
        (void)m_jit->execute(*cb, cpu);
        ASSERT_FALSE(cpu.pending_raise.active);
    }
    u32 seen = 0;
    ASSERT_TRUE(m_memory->read_bytes(kDataA + 0x40,
                                     reinterpret_cast<u8*>(&seen), 4));
    ASSERT_EQ(seen, 0x11111111u) << "first store did not land";

    ASSERT_TRUE(m_memory->protect_region(kDataA, kDataSize,
                                         MemoryProtection::Read));
    {
        CpuState cpu;
        init_cpu(cpu);
        cpu.gpr.set64(RBX, kDataA + 0x40);
        cpu.gpr.set64(RAX, 0x22222222);
        (void)m_jit->execute(*cb, cpu);
        EXPECT_TRUE(cpu.pending_raise.active)
            << "store to a newly read-only region went through a stale "
               "write-region slot";
    }

    ASSERT_TRUE(m_memory->protect_region(kDataA, kDataSize,
                                         MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->read_bytes(kDataA + 0x40,
                                     reinterpret_cast<u8*>(&seen), 4));
    EXPECT_EQ(seen, 0x11111111u)
        << "read-only region was written through the inline path";
}

// RW↔RX (W^X) keeps Read. Retracting load caches on every protect forces
// get_region_info on the next block and convoys on the region map lock.
TEST_F(JitX64InlineMemTest, ProtectToExecuteKeepsPublishedReadRegion) {
    ASSERT_TRUE(m_memory->write_u32(kDataA + 0x40, 0xA1B2C3D4u));
    const auto load = load32_rbx();
    ASSERT_TRUE(m_memory->write_bytes(kCodeBase, load.data(), load.size()));
    BasicBlock load_block = build_block(load, kCodeBase);
    CompiledBlock* load_cb = m_jit->compile(load_block);
    ASSERT_NE(load_cb, nullptr);

    {
        CpuState cpu;
        init_cpu(cpu);
        cpu.gpr.set64(RBX, kDataA + 0x40);
        (void)m_jit->execute(*load_cb, cpu);
        ASSERT_FALSE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.gpr.get32(RAX), 0xA1B2C3D4u);
    }

    ASSERT_TRUE(m_memory->protect_region(
        kDataA, kDataSize,
        MemoryProtection::Read | MemoryProtection::Execute));

    {
        CpuState cpu;
        init_cpu(cpu);
        cpu.gpr.set64(RBX, kDataA + 0x40);
        cpu.gpr.set64(RAX, 0);
        (void)m_jit->execute(*load_cb, cpu);
        EXPECT_FALSE(cpu.pending_raise.active)
            << "load after RW→RX protect missed a still-readable region";
        EXPECT_EQ(cpu.gpr.get32(RAX), 0xA1B2C3D4u);
    }

    const auto store = store32_rbx();
    ASSERT_TRUE(m_memory->write_bytes(kCodeBase + 0x20, store.data(),
                                      store.size()));
    BasicBlock store_block = build_block(store, kCodeBase + 0x20);
    CompiledBlock* store_cb = m_jit->compile(store_block);
    ASSERT_NE(store_cb, nullptr);
    {
        CpuState cpu;
        init_cpu(cpu);
        cpu.gpr.set64(RBX, kDataA + 0x40);
        cpu.gpr.set64(RAX, 0x22222222);
        (void)m_jit->execute(*store_cb, cpu);
        EXPECT_TRUE(cpu.pending_raise.active)
            << "store after RW→RX protect went through a stale write slot";
    }
}

// A protect shootdown is ranged: revoking access to one region must retract
// only the slots it overlaps. Another region's published load slot keeps
// serving inline (no helper read), while the revoked region faults exactly as
// the interpreter does.
TEST_F(JitX64InlineMemTest, ProtectOneRegionKeepsOtherRegionsLoadSlot) {
    constexpr GuestAddr kDataC = kDataB + 0x10000;  // a different slot index
    static_assert(x64_slot_index(kDataC) != x64_slot_index(kDataA));
    ASSERT_TRUE(m_memory->map_region(kDataC, 0x1000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u64(kDataA + 0x40, 0x1111222233334444ull));
    ASSERT_TRUE(m_memory->write_u64(kDataC + 0x40, 0x5555666677778888ull));

    const auto code = load64_rbx();
    ASSERT_TRUE(m_memory->write_bytes(kCodeBase, code.data(), code.size()));
    BasicBlock block = build_block(code, kCodeBase);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    auto load = [&](GuestAddr addr, bool& raised) {
        CpuState cpu;
        init_cpu(cpu);
        cpu.gpr.set64(RBX, addr);
        cpu.gpr.set64(RAX, 0);
        (void)m_jit->execute(*cb, cpu);
        raised = cpu.pending_raise.active;
        return cpu.gpr.get64(RAX);
    };
    bool raised = false;
    // Publish both regions' slots through the helper.
    ASSERT_EQ(load(kDataA + 0x40, raised), 0x1111222233334444ull);
    ASSERT_FALSE(raised);
    ASSERT_EQ(load(kDataC + 0x40, raised), 0x5555666677778888ull);
    ASSERT_FALSE(raised);

    diagnostics::perf_window_enable_for_tests();
    diagnostics::perf_window_start();
    const u64 published = diagnostics::perf_window_snapshot().helper_reads;
    EXPECT_EQ(load(kDataC + 0x40, raised), 0x5555666677778888ull);
    EXPECT_FALSE(raised);
    ASSERT_EQ(diagnostics::perf_window_snapshot().helper_reads, published)
        << "a published load slot did not serve the access inline";

    ASSERT_TRUE(m_memory->protect_region(kDataA, kDataSize,
                                         MemoryProtection::None));
    EXPECT_EQ(load(kDataC + 0x40, raised), 0x5555666677778888ull);
    EXPECT_FALSE(raised);
    EXPECT_EQ(diagnostics::perf_window_snapshot().helper_reads, published)
        << "protecting an unrelated region retracted this region's slot";

    (void)load(kDataA + 0x40, raised);
    EXPECT_TRUE(raised) << "load from a PAGE_NOACCESS region went through a "
                           "stale load slot";
    diagnostics::perf_window_stop();
    diagnostics::perf_window_reset_for_tests();

    // The interpreter is the reference for both outcomes. (check_parity's
    // memory snapshot cannot read a no-access region, so run it directly.)
    for (const auto [addr, faults] : {std::pair{kDataA + 0x40, true},
                                      std::pair{kDataC + 0x40, false}}) {
        CpuState icpu;
        init_cpu(icpu);
        icpu.gpr.set64(RBX, addr);
        icpu.gpr.set64(RAX, 0);
        Interpreter interp(icpu, *m_memory);
        const ExecResult r = interp.run(block.instruction_count);
        EXPECT_EQ(r == ExecResult::Exception || icpu.pending_raise.active,
                  faults) << std::hex << addr;
        if (!faults) EXPECT_EQ(icpu.gpr.get64(RAX), 0x5555666677778888ull);
    }
    ASSERT_TRUE(m_memory->protect_region(kDataA, kDataSize,
                                         MemoryProtection::ReadWrite));
}

// ---------------------------------------------------------------------------
// Self-modifying code. The inline store skips notify_executable_write(), so a
// region that holds compiled code must never be published. One write is safe
// on its own (it misses an empty slot and reaches the helper); the hazard is
// the helper publishing afterwards, which would let the *next* write skip
// notification. Two cached blocks on different pages make that observable.
// ---------------------------------------------------------------------------

TEST_F(JitX64InlineMemTest, RepeatedStoresIntoCodeRegionKeepInvalidating) {
    const std::vector<u8> victim = {0x48, 0xC7, 0xC0, 0x11, 0x22, 0x33, 0x44,
                                    0xC3};  // MOV RAX, imm32 ; RET
    const GuestAddr page0 = kCodeBase;
    const GuestAddr page2 = kCodeBase + 0x2000;

    auto cache_victim_at = [&](GuestAddr where) {
        ASSERT_TRUE(m_memory->write_bytes(where, victim.data(), victim.size()));
        BasicBlock b = build_block(victim, where);
        ASSERT_GT(b.instruction_count, 0u);
        ASSERT_NE(m_jit->compile(b), nullptr);
        m_cache->insert(b);
    };

    cache_victim_at(page0);
    cache_victim_at(page2);
    ASSERT_EQ(m_cache->size(), 2u);
    ASSERT_FALSE(m_cache->range_is_code_free(kCodeBase, kCodeSize));

    // A storing block on a third page writes to page0 and then to page2, so
    // executing it does not itself add code to the pages under test.
    std::vector<u8> code;
    code.push_back(0x48);  // MOV RBX, page0
    code.push_back(0xBB);
    for (int i = 0; i < 8; ++i) {
        code.push_back(static_cast<u8>((page0 >> (i * 8)) & 0xFF));
    }
    for (u8 b : store32_rbx()) code.push_back(b);
    code.push_back(0x48);  // MOV RBX, page2
    code.push_back(0xBB);
    for (int i = 0; i < 8; ++i) {
        code.push_back(static_cast<u8>((page2 >> (i * 8)) & 0xFF));
    }
    for (u8 b : store32_rbx()) code.push_back(b);

    const GuestAddr store_at = kCodeBase + 0x1000;
    ASSERT_TRUE(m_memory->write_bytes(store_at, code.data(), code.size()));
    BasicBlock sblock = build_block(code, store_at);
    ASSERT_EQ(sblock.instruction_count, 4u);

    CpuState cpu;
    init_cpu(cpu);
    cpu.rip = store_at;
    cpu.gpr.set64(RAX, 0x99999999);
    CompiledBlock* scb = m_jit->compile(sblock);
    ASSERT_NE(scb, nullptr);
    (void)m_jit->execute(*scb, cpu);

    EXPECT_EQ(m_cache->size(), 0u)
        << "a self-modifying write skipped SMC invalidation -- the write "
           "helper published a code-bearing region into the inline store "
           "fast path";
}

// ---------------------------------------------------------------------------
// The shape this optimization exists to serve: a sequential fill, then a
// sequential read-back, verified byte by byte so an off-by-one in the offset
// computation shows up as a diff rather than as a lucky pass.
// ---------------------------------------------------------------------------

TEST_F(JitX64InlineMemTest, SequentialFillMatchesInterpreter) {
    std::vector<u8> code;
    for (u32 i = 0; i < 64; ++i) {
        for (u8 b : store8_rbx()) code.push_back(b);
        code.push_back(0x48);  // INC RBX
        code.push_back(0xFF);
        code.push_back(0xC3);
    }
    check_parity(code, at(kDataA + 0x100, 0xA5ull));
}

TEST_F(JitX64InlineMemTest, SequentialCopyMatchesInterpreter) {
    for (u32 i = 0; i < 64; ++i) {
        ASSERT_TRUE(m_memory->write_u8(kDataA + 0x200 + i,
                                       static_cast<u8>(i * 7 + 1)));
    }
    // Four iterations of: MOV RAX,[RBX] ; MOV [RBX+0x800],RAX ; ADD RBX,8 —
    // an inline load feeding an inline store, the decompressor's inner shape.
    std::vector<u8> code;
    for (u32 i = 0; i < 4; ++i) {
        for (u8 b : load64_rbx()) code.push_back(b);
        code.push_back(0x48);  // MOV [RBX+0x800], RAX
        code.push_back(0x89);
        code.push_back(0x83);
        for (int j = 0; j < 4; ++j) {
            code.push_back(static_cast<u8>((0x800u >> (j * 8)) & 0xFF));
        }
        code.push_back(0x48);  // ADD RBX, 8
        code.push_back(0x83);
        code.push_back(0xC3);
        code.push_back(0x08);
    }
    check_parity(code, at(kDataA + 0x200, 0));
}

}  // namespace

// ============================================================================
// Direct-mapped translation cache
//
// Emitted accesses resolve through a table indexed by guest address bits, so
// two regions a multiple of the index span apart share one entry, and a region
// larger than that span occupies several. Both cases must stay coherent: the
// entry records the whole region it resolved to, and every probe re-checks the
// bounds before using the host pointer.
// ============================================================================

TEST_F(JitX64InlineMemTest, AliasingRegionsAlternateWithoutCrossTalk) {
    static_assert(x64_slot_index(kDataA) == x64_slot_index(kDataB),
                  "these regions are meant to collide in the slot table");
    // Alternate stores and loads between two regions sharing a table entry.
    // Reading through the other region's host pointer would surface as a
    // wrong value in ESI/EDI or a store landing in the wrong region.
    //   MOV [RAX],EBX ; MOV [RCX],EDX ; MOV ESI,[RAX] ; MOV EDI,[RCX]
    std::vector<u8> code = {
        0x89, 0x18, 0x89, 0x11, 0x8B, 0x30, 0x8B, 0x39,
        0x89, 0x18, 0x89, 0x11, 0x8B, 0x30, 0x8B, 0x39,
    };
    check_parity(code, [](CpuState& cpu) {
        cpu.gpr.set64(RAX, kDataA);
        cpu.gpr.set64(static_cast<u8>(RegIndex::RCX), kDataB);
        cpu.gpr.set64(RBX, 0x11111111u);
        cpu.gpr.set64(static_cast<u8>(RegIndex::RDX), 0x22222222u);
    });
}

TEST_F(JitX64InlineMemTest, AliasingRegionAndStackAlternateWithoutCrossTalk) {
    // The extraction profile interleaves stack and buffer accesses, which is
    // the pattern a two-slot cache could not hold at all.
    //   MOV [RAX],EBX ; PUSH RBX ; MOV ESI,[RAX] ; POP RDI
    std::vector<u8> code = {
        0x89, 0x18, 0x53, 0x8B, 0x30, 0x5F,
        0x89, 0x18, 0x53, 0x8B, 0x30, 0x5F,
    };
    check_parity(code, [](CpuState& cpu) {
        cpu.gpr.set64(RAX, kDataA);
        cpu.gpr.set64(RBX, 0xABCDEF01u);
    });
}

TEST_F(JitX64InlineMemTest, OneRegionSpanningManyGranulesStaysCoherent) {
    // A region wider than the whole index space: every granule maps to some
    // entry, and each entry must still describe the full region so that an
    // access anywhere in it is in bounds.
    constexpr GuestAddr kBig = 0x700000000ull;
    constexpr usize kBigSize = 4u << 20;
    ASSERT_TRUE(m_memory->map_region(kBig, kBigSize,
                                     MemoryProtection::ReadWrite));

    const std::vector<u8> code = {0x89, 0x18};  // MOV [RAX], EBX
    ASSERT_TRUE(m_memory->write_bytes(kCodeBase, code.data(), code.size()));
    BasicBlock block = build_block(code, kCodeBase);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    // Two passes over the same offsets: the second finds entries already
    // populated, including entries republished by an intervening granule.
    for (int pass = 0; pass < 2; ++pass) {
        for (usize offset = 0; offset < kBigSize; offset += 64u * 1024u) {
            CpuState cpu;
            init_cpu(cpu);
            cpu.gpr.set64(RAX, kBig + offset);
            cpu.gpr.set64(RBX, static_cast<u32>(offset ^ 0x5A5Au) +
                               static_cast<u32>(pass));
            ASSERT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
            ASSERT_FALSE(cpu.pending_raise.active)
                << "offset 0x" << std::hex << offset;
        }
        for (usize offset = 0; offset < kBigSize; offset += 64u * 1024u) {
            auto value = m_memory->read_u32(kBig + offset);
            ASSERT_TRUE(value.is_ok());
            EXPECT_EQ(value.value(),
                      static_cast<u32>(offset ^ 0x5A5Au) +
                          static_cast<u32>(pass))
                << "granule at offset 0x" << std::hex << offset
                << " read back the wrong value";
        }
    }
}

TEST_F(JitX64InlineMemTest, UnmapAfterPublishDoesNotLeaveStaleTranslation) {
    // A freed region's host pointer must not survive in the table: the next
    // access to that address has to fault rather than write through it.
    constexpr GuestAddr kTemp = 0x800000000ull;
    ASSERT_TRUE(m_memory->map_region(kTemp, 0x1000,
                                     MemoryProtection::ReadWrite));

    const std::vector<u8> code = {0x89, 0x18};  // MOV [RAX], EBX
    ASSERT_TRUE(m_memory->write_bytes(kCodeBase, code.data(), code.size()));
    BasicBlock block = build_block(code, kCodeBase);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    CpuState warm;
    init_cpu(warm);
    warm.gpr.set64(RAX, kTemp);
    warm.gpr.set64(RBX, 0x1234u);
    ASSERT_EQ(m_jit->execute(*compiled, warm), JitExecResult::Continue);
    ASSERT_FALSE(warm.pending_raise.active);

    ASSERT_TRUE(m_memory->unmap_region(kTemp, 0x1000));

    CpuState after;
    init_cpu(after);
    after.gpr.set64(RAX, kTemp);
    after.gpr.set64(RBX, 0x5678u);
    (void)m_jit->execute(*compiled, after);
    EXPECT_TRUE(after.pending_raise.active)
        << "store reached a freed region through a stale cached translation";
}

// ---------------------------------------------------------------------------
// BT / BTS / BTR / BTC with a memory bit base. Intel SDM Vol. 2: a register
// bit offset is signed and selects the dword (word) at
// BitBase + (BitOffset DIV operand_size) * operand_size_in_bytes, so the byte
// offset has to be folded into the *full* effective address. Folding it with a
// 32-bit ADD truncated every bit base above 4 GB, turning BTS/BTR/BTC into a
// silent write into unrelated low memory.
// ---------------------------------------------------------------------------

TEST_F(JitX64InlineMemTest, BtsHighAddressInRangeOffsetMatchesInterpreter) {
    ASSERT_TRUE(m_memory->write_u64(kDataA + 0x40, 0));
    // BTS [RBX], EAX ; SETC AL — carries the read-out bit into a compared GPR.
    check_parity({0x0F, 0xAB, 0x03, 0x0F, 0x92, 0xC0}, at(kDataA + 0x40, 5));
}

TEST_F(JitX64InlineMemTest, BtsHighAddressDoesNotAliasLowShadowRegion) {
    ASSERT_TRUE(m_memory->write_u64(kDataA + 0x40, 0));
    ASSERT_TRUE(m_memory->write_u64(kShadow + 0x40, 0x5555555555555555ull));
    check_parity({0x0F, 0xAB, 0x03}, at(kDataA + 0x40, 5));

    u64 shadow = 0;
    ASSERT_TRUE(m_memory->read_bytes(kShadow + 0x40,
                                     reinterpret_cast<u8*>(&shadow), 8));
    EXPECT_EQ(shadow, 0x5555555555555555ull)
        << "BTS on a >4GB bit base was truncated onto its low 32 bits";
}

// Offset past the addressed dword: the access moves to BitBase + 12.
TEST_F(JitX64InlineMemTest, BtsHighAddressFarPositiveOffsetMatchesInterpreter) {
    ASSERT_TRUE(m_memory->write_u64(kDataA + 0x40, 0));
    ASSERT_TRUE(m_memory->write_u64(kDataA + 0x48, 0));
    check_parity({0x0F, 0xAB, 0x03, 0x0F, 0x92, 0xC0}, at(kDataA + 0x40, 100));
}

// Negative offset: (-33 DIV 32) = -2 dwords, bit 31 of BitBase - 8.
TEST_F(JitX64InlineMemTest, BtrHighAddressNegativeOffsetMatchesInterpreter) {
    ASSERT_TRUE(m_memory->write_u64(kDataA + 0x38, 0xFFFFFFFFFFFFFFFFull));
    // BTR [RBX], EAX ; SETC AL
    check_parity({0x0F, 0xB3, 0x03, 0x0F, 0x92, 0xC0},
                 at(kDataA + 0x40, 0xFFFFFFDFull));
}

TEST_F(JitX64InlineMemTest, BtcHighAddressMatchesInterpreter) {
    ASSERT_TRUE(m_memory->write_u64(kDataA + 0x40, 0xA5A5A5A5A5A5A5A5ull));
    // BTC [RBX], EAX ; SETC AL
    check_parity({0x0F, 0xBB, 0x03, 0x0F, 0x92, 0xC0}, at(kDataA + 0x40, 70));
}

TEST_F(JitX64InlineMemTest, BtHighAddressReadOnlyFormMatchesInterpreter) {
    ASSERT_TRUE(m_memory->write_u64(kDataA + 0x40, 0x0000000800000000ull));
    // BT [RBX], EAX ; SETC AL
    check_parity({0x0F, 0xA3, 0x03, 0x0F, 0x92, 0xC0}, at(kDataA + 0x40, 35));
}

// 16-bit form: (BitOffset DIV 16) * 2 bytes, same 64-bit fold.
TEST_F(JitX64InlineMemTest, Bts16HighAddressMatchesInterpreter) {
    ASSERT_TRUE(m_memory->write_u64(kDataA + 0x40, 0));
    // BTS [RBX], AX ; SETC AL
    check_parity({0x66, 0x0F, 0xAB, 0x03, 0x0F, 0x92, 0xC0},
                 at(kDataA + 0x40, 40));
}

TEST_F(JitX64InlineMemTest, Bts16HighAddressDoesNotAliasLowShadowRegion) {
    ASSERT_TRUE(m_memory->write_u64(kDataA + 0x40, 0));
    ASSERT_TRUE(m_memory->write_u64(kShadow + 0x40, 0x5555555555555555ull));
    check_parity({0x66, 0x0F, 0xAB, 0x03}, at(kDataA + 0x40, 40));

    u64 shadow = 0;
    ASSERT_TRUE(m_memory->read_bytes(kShadow + 0x40,
                                     reinterpret_cast<u8*>(&shadow), 8));
    EXPECT_EQ(shadow, 0x5555555555555555ull)
        << "16-bit BTS on a >4GB bit base was truncated onto its low 32 bits";
}

// Spotify FileDescriptor wrapper ctor: xor edi,edi then MOV [rax+disp], rdi
// for every qword through +0xD8. A 16-bit or skipped store at +0x70 leaves
// 0xFE00xxxx and CreateFlatAlloc sign-extends it into a huge operator new.
TEST_F(JitX64InlineMemTest, QwordZeroStoresClearFeFillThrough0x70) {
    std::vector<u8> fe(0xE0, 0xFE);
    ASSERT_TRUE(m_memory->write_bytes(kDataA, fe.data(), fe.size()));

    // Bytes from Spotify.dll 0x16bc2c2..0x16bc355 (rdi already 0).
    const std::vector<u8> stores = {
        0x48, 0x89, 0x38, 0x48, 0x89, 0x78, 0x08, 0x48, 0x89, 0x78, 0x10,
        0x48, 0x89, 0x78, 0x18, 0x48, 0x89, 0x78, 0x20, 0x48, 0x89, 0x78, 0x28,
        0x48, 0x89, 0x78, 0x30, 0x48, 0x89, 0x78, 0x38, 0x48, 0x89, 0x78, 0x40,
        0x48, 0x89, 0x78, 0x48, 0x48, 0x89, 0x78, 0x50, 0x48, 0x89, 0x78, 0x58,
        0x48, 0x89, 0x78, 0x60, 0x48, 0x89, 0x78, 0x68, 0x48, 0x89, 0x78, 0x70,
        0x48, 0x89, 0x78, 0x78, 0x48, 0x89, 0xB8, 0x80, 0x00, 0x00, 0x00,
        0x48, 0x89, 0xB8, 0x88, 0x00, 0x00, 0x00, 0x48, 0x89, 0xB8, 0x90, 0x00,
        0x00, 0x00, 0x48, 0x89, 0xB8, 0x98, 0x00, 0x00, 0x00, 0x48, 0x89, 0xB8,
        0xA0, 0x00, 0x00, 0x00, 0x48, 0x89, 0xB8, 0xA8, 0x00, 0x00, 0x00,
        0x48, 0x89, 0xB8, 0xB0, 0x00, 0x00, 0x00, 0x48, 0x89, 0xB8, 0xB8, 0x00,
        0x00, 0x00, 0x48, 0x89, 0xB8, 0xC0, 0x00, 0x00, 0x00, 0x48, 0x89, 0xB8,
        0xC8, 0x00, 0x00, 0x00, 0x48, 0x89, 0xB8, 0xD0, 0x00, 0x00, 0x00,
        0x48, 0x89, 0xB8, 0xD8, 0x00, 0x00, 0x00,
    };
    check_parity(stores, [](CpuState& c) {
        c.gpr.set64(RAX, kDataA);
        c.gpr.set64(static_cast<u8>(RegIndex::RDI), 0);
    });
}

TEST_F(JitX64InlineMemTest, QwordZeroStoresClearFeFillOnLowHeapAddress) {
    constexpr GuestAddr kLowHeap = 0x2A2000;
    ASSERT_TRUE(m_memory->map_region(kLowHeap, 0x1000, MemoryProtection::ReadWrite));
    std::vector<u8> fe(0xE0, 0xFE);
    ASSERT_TRUE(m_memory->write_bytes(kLowHeap, fe.data(), fe.size()));

    const std::vector<u8> stores = {
        0x48, 0x89, 0x38, 0x48, 0x89, 0x78, 0x08, 0x48, 0x89, 0x78, 0x10,
        0x48, 0x89, 0x78, 0x18, 0x48, 0x89, 0x78, 0x20, 0x48, 0x89, 0x78, 0x28,
        0x48, 0x89, 0x78, 0x30, 0x48, 0x89, 0x78, 0x38, 0x48, 0x89, 0x78, 0x40,
        0x48, 0x89, 0x78, 0x48, 0x48, 0x89, 0x78, 0x50, 0x48, 0x89, 0x78, 0x58,
        0x48, 0x89, 0x78, 0x60, 0x48, 0x89, 0x78, 0x68, 0x48, 0x89, 0x78, 0x70,
        0x48, 0x89, 0x78, 0x78,
    };
    ASSERT_TRUE(m_memory->write_bytes(kCodeBase, stores.data(), stores.size()));
    BasicBlock block = build_block(stores, kCodeBase);
    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set64(RAX, kLowHeap);
    cpu.gpr.set64(static_cast<u8>(RegIndex::RDI), 0);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    (void)m_jit->execute(*cb, cpu);

    u64 at70 = 0xFFFFFFFFFFFFFFFFull;
    ASSERT_TRUE(m_memory->read_bytes(kLowHeap + 0x70,
                                     reinterpret_cast<u8*>(&at70), 8));
    EXPECT_EQ(at70, 0ull)
        << "low-heap ctor store left stale 0xFE fill at +0x70";
}

// ---------------------------------------------------------------------------
// SMC end to end: a guest that overwrites a code page it has already executed
// must go on to run the NEW bytes. The store must not take the inline path (it
// would skip notify_executable_write entirely), the cached translation must be
// retired, and a re-dispatch must produce the rewritten instruction's result.
//
// jit_code_pages_overlap() is the gate that decides all three. It grew a
// coarse span envelope and a repeat memo for speed; both are conservative only
// in the over-report direction, and this test is what proves it.
// ---------------------------------------------------------------------------

TEST_F(JitX64InlineMemTest, SelfModifyingCodeRunsTheRewrittenBytes) {
    // MOV RAX, imm32 ; RET -- the imm32 sits at victim+3.
    const std::vector<u8> victim = {0x48, 0xC7, 0xC0, 0x44, 0x33, 0x22, 0x11,
                                    0xC3};
    const GuestAddr victim_at = kCodeBase;
    ASSERT_TRUE(m_memory->write_bytes(victim_at, victim.data(), victim.size()));
    BasicBlock vblock = build_block(victim, victim_at);
    ASSERT_GT(vblock.instruction_count, 0u);
    CompiledBlock* vcb = m_jit->compile(vblock);
    ASSERT_NE(vcb, nullptr);
    m_cache->insert(vblock);
    ASSERT_EQ(m_cache->size(), 1u);

    {
        CpuState cpu;
        init_cpu(cpu);
        cpu.rip = victim_at;
        (void)m_jit->execute(*vcb, cpu);
        ASSERT_EQ(cpu.gpr.get64(RAX), 0x11223344ull)
            << "the original translation did not run";
    }

    // A storing block on another page rewrites the imm32 to 0x55667788.
    std::vector<u8> code;
    code.push_back(0x48);  // MOV RBX, victim_at + 3
    code.push_back(0xBB);
    for (int i = 0; i < 8; ++i) {
        code.push_back(static_cast<u8>(((victim_at + 3) >> (i * 8)) & 0xFF));
    }
    for (u8 b : store32_rbx()) code.push_back(b);  // MOV [RBX], EAX

    const GuestAddr store_at = kCodeBase + 0x1000;
    ASSERT_TRUE(m_memory->write_bytes(store_at, code.data(), code.size()));
    BasicBlock sblock = build_block(code, store_at);
    ASSERT_EQ(sblock.instruction_count, 2u);

    CpuState cpu;
    init_cpu(cpu);
    cpu.rip = store_at;
    cpu.gpr.set64(RAX, 0x55667788ull);
    CompiledBlock* scb = m_jit->compile(sblock);
    ASSERT_NE(scb, nullptr);
    (void)m_jit->execute(*scb, cpu);

    // 1. The new bytes are actually in guest memory.
    u32 imm = 0;
    ASSERT_TRUE(m_memory->read_bytes(victim_at + 3,
                                     reinterpret_cast<u8*>(&imm), 4));
    EXPECT_EQ(imm, 0x55667788u) << "the self-modifying store did not land";

    // 2. The stale translation was retired, so a dispatch re-translates.
    EXPECT_EQ(m_cache->size(), 0u)
        << "the rewritten code page kept its cached translation -- the store "
           "helper published a code-bearing region into the inline store fast "
           "path, or jit_code_pages_overlap() under-reported it";

    // 3. Re-decoding from guest memory yields the NEW instruction, and
    //    executing it gives the rewritten result. (The JIT deliberately
    //    refuses to re-compile a page it just invalidated and routes the
    //    re-dispatch to the interpreter, so the interpreter is the engine the
    //    guest actually observes here.)
    std::vector<u8> now(victim.size());
    ASSERT_TRUE(m_memory->read_bytes(victim_at, now.data(), now.size()));
    BasicBlock again = build_block(now, victim_at);
    ASSERT_GT(again.instruction_count, 0u);
    CpuState after;
    init_cpu(after);
    after.rip = victim_at;
    after.gpr.set64(RAX, 0xDEADDEADull);
    {
        Interpreter interp(after, *m_memory);
        (void)interp.run(again.instruction_count);
    }
    EXPECT_EQ(after.gpr.get64(RAX), 0x55667788ull)
        << "the guest did not observe the bytes it just wrote to its own code";
}

// ---------------------------------------------------------------------------
// A MEM_WRITE_WATCH region must never reach the inline store fast path: the
// emitted store writes host memory with no hook of any kind, so a page written
// through it would be reported CLEAN by GetWriteWatch -- and a guest GC frees
// live objects on a clean lie (getwritewatch_gc_landmine).
//
// Observable: two stores to two different pages of a watched region. If the
// first store publishes the region, the second is inline and its page is never
// marked dirty.
// ---------------------------------------------------------------------------

TEST_F(JitX64InlineMemTest, WatchedRegionIsNeverPublishedToTheInlinePath) {
    constexpr u64 kPages = kDataSize / 4096;
    std::vector<u8> dirty((kPages + 7) / 8, 0);
    std::vector<u8> armed((kPages + 7) / 8, 0);
    write_watch::register_region(kDataA, kDataSize, dirty.data(), armed.data(),
                                 /*executable=*/false);
    struct Unregister {
        ~Unregister() { write_watch::unregister_region(kDataA); }
    } unregister_on_exit;

    const auto code = store32_rbx();
    ASSERT_TRUE(m_memory->write_bytes(kCodeBase, code.data(), code.size()));
    BasicBlock block = build_block(code, kCodeBase);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);

    // Page 0 first, then page 1. Distinct pages so the second store cannot be
    // covered by the first one's dirty bit.
    const GuestAddr targets[2] = {kDataA + 0x40, kDataA + 0x1040};
    for (int i = 0; i < 2; ++i) {
        CpuState cpu;
        init_cpu(cpu);
        cpu.gpr.set64(RBX, targets[i]);
        cpu.gpr.set64(RAX, 0xA0A0A0A0ull + static_cast<u64>(i));
        (void)m_jit->execute(*cb, cpu);
        ASSERT_FALSE(cpu.pending_raise.active) << "store " << i << " faulted";
    }

    EXPECT_TRUE(write_watch::test_bit(dirty.data(), 0))
        << "first store into a watched region was not recorded";
    EXPECT_TRUE(write_watch::test_bit(dirty.data(), 1))
        << "the second store into a watched region was not recorded: the "
           "region was published into the inline store fast path, so "
           "GetWriteWatch would report a written page as clean";
}

TEST_F(JitX64InlineMemTest,
       ActivatingWriteWatchRetractsAnAlreadyPublishedHighStoreSlot) {
    const auto code = store32_rbx();
    ASSERT_TRUE(m_memory->write_bytes(kCodeBase, code.data(), code.size()));
    BasicBlock block = build_block(code, kCodeBase);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);

    // The first checked store publishes kDataA while no watch exists.
    CpuState first;
    init_cpu(first);
    first.gpr.set64(RBX, kDataA + 0x40);
    first.gpr.set64(RAX, 0x11111111u);
    (void)m_jit->execute(*cb, first);
    ASSERT_FALSE(first.pending_raise.active);

    constexpr u64 kPages = kDataSize / 4096;
    std::vector<u8> dirty((kPages + 7) / 8, 0);
    std::vector<u8> armed((kPages + 7) / 8, 0);
    write_watch::register_region(kDataA, kDataSize, dirty.data(), armed.data(),
                                 /*executable=*/false);
    struct Unregister {
        ~Unregister() { write_watch::unregister_region(kDataA); }
    } unregister_on_exit;

    // Registration changes the table generation. execute() must retract the
    // old >4 GiB slot before this store, forcing record_store instead of the
    // host-direct path that was valid for the first execution.
    CpuState second;
    init_cpu(second);
    second.gpr.set64(RBX, kDataA + 0x1040);
    second.gpr.set64(RAX, 0x22222222u);
    (void)m_jit->execute(*cb, second);
    ASSERT_FALSE(second.pending_raise.active);
    EXPECT_TRUE(write_watch::test_bit(dirty.data(), 1));
}
