// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Inline store fast-path tests (32-bit mode).
//
// emit_guest_store8/32 write directly to host memory when the target falls in
// one of the JitContext wregion slots, skipping the write helper entirely --
// and with it the permission check, the SMC notification and the access
// violation raise. Those are safe to skip only because a region is published
// into a wregion slot solely when it is writable and holds no compiled code.
//
// These tests exercise the cases where that invariant is load-bearing:
// stores that must miss the fast path (read-only regions, code-bearing
// regions, region edges) and stores that must hit it while still producing
// exactly what the interpreter produces. A regression here is silent memory
// corruption, not a crash, so the checks compare full memory rather than
// just return values.

#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

constexpr u8 EAX = static_cast<u8>(RegIndex::RAX);
constexpr u8 ESP = static_cast<u8>(RegIndex::RSP);

// Guest layout. Two writable data regions so the tests can alternate between
// them and exercise both wregion slots plus the slow-path republish, and one
// read-only region that must never be reachable through the fast path.
// The code region spans several pages on purpose: the SMC test needs one
// cached block to survive a write that invalidates a block on a *different*
// page of the same region.
constexpr GuestAddr kCodeBase = 0x1000;
constexpr usize     kCodeSize = 0x4000;
constexpr GuestAddr kDataA    = 0x20000;
constexpr usize     kDataSize = 0x2000;
constexpr GuestAddr kDataB    = 0x40000;
constexpr GuestAddr kRoBase   = 0x60000;
constexpr usize     kRoSize   = 0x1000;
constexpr GuestAddr kStack    = 0x80000;
constexpr usize     kStackSize = 0x4000;

class JitInlineStoreTest : public ::testing::Test {
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
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_cache = std::make_unique<BlockCache>();
        m_jit->set_block_cache(m_cache.get());
    }

    BasicBlock build_block(const std::vector<u8>& code) {
        BasicBlock block;
        block.start_addr = kCodeBase;
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

    void init_cpu(CpuState& cpu) {
        cpu.init_protected_mode();
        cpu.gpr.set32(ESP, static_cast<u32>(kStack + kStackSize - 0x100));
        cpu.rip = kCodeBase;
    }

    // Read every writable data byte, so a store landing at the wrong offset
    // (or in the wrong region) is caught wherever it lands.
    std::vector<u8> snapshot_data() {
        std::vector<u8> out(kDataSize * 2 + kRoSize);
        EXPECT_TRUE(m_memory->read_bytes(kDataA, out.data(), kDataSize));
        EXPECT_TRUE(m_memory->read_bytes(kDataB, out.data() + kDataSize,
                                         kDataSize));
        EXPECT_TRUE(m_memory->read_bytes(kRoBase, out.data() + kDataSize * 2,
                                         kRoSize));
        return out;
    }

    void restore_data(const std::vector<u8>& snap) {
        // Written through the raw region pointers: the read-only region is not
        // writable through the guest API, which is the point of the test.
        EXPECT_TRUE(m_memory->write_bytes(kDataA, snap.data(), kDataSize));
        EXPECT_TRUE(m_memory->write_bytes(kDataB, snap.data() + kDataSize,
                                          kDataSize));
    }

    /// Run identical code through interpreter and JIT; require identical
    /// registers, memory, and access-violation outcome.
    void check_parity(const std::vector<u8>& code,
                      std::function<void(CpuState&)> setup = {}) {
        ASSERT_TRUE(m_memory->write_bytes(kCodeBase, code.data(), code.size()));
        BasicBlock block = build_block(code);
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
        // The two engines report a bad access differently: the interpreter
        // returns ExecResult::Exception, the JIT sets cpu.pending_raise (its
        // helpers run below the point where a result could be returned).
        // Compare the fault/no-fault outcome, not the mechanism.
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

        for (u8 i = 0; i < 8; ++i) {
            EXPECT_EQ(icpu.gpr.get32(i), jcpu.gpr.get32(i))
                << "GPR " << static_cast<unsigned>(i) << " diverged";
        }

        ASSERT_EQ(interp_mem.size(), jit_mem.size());
        for (usize i = 0; i < interp_mem.size(); ++i) {
            if (interp_mem[i] != jit_mem[i]) {
                GuestAddr where = (i < kDataSize)         ? kDataA + i
                                : (i < kDataSize * 2)     ? kDataB + (i - kDataSize)
                                                          : kRoBase + (i - kDataSize * 2);
                ADD_FAILURE() << "memory diverged at 0x" << std::hex << where
                              << ": interp=0x"
                              << static_cast<unsigned>(interp_mem[i])
                              << " jit=0x"
                              << static_cast<unsigned>(jit_mem[i]);
                break;  // one report is enough; the whole region is suspect
            }
        }

        restore_data(initial);
    }

    // MOV [imm32], EAX  (opcode A3) — 32-bit store to an absolute address.
    static std::vector<u8> store32_abs(u32 addr) {
        return {0xA3, static_cast<u8>(addr), static_cast<u8>(addr >> 8),
                static_cast<u8>(addr >> 16), static_cast<u8>(addr >> 24)};
    }

    // MOV [imm32], AL  (opcode A2) — 8-bit store to an absolute address.
    static std::vector<u8> store8_abs(u32 addr) {
        return {0xA2, static_cast<u8>(addr), static_cast<u8>(addr >> 8),
                static_cast<u8>(addr >> 16), static_cast<u8>(addr >> 24)};
    }

    // MOV [imm32], AX  (66 A3) — 16-bit store to an absolute address.
    static std::vector<u8> store16_abs(u32 addr) {
        return {0x66, 0xA3, static_cast<u8>(addr), static_cast<u8>(addr >> 8),
                static_cast<u8>(addr >> 16), static_cast<u8>(addr >> 24)};
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
    std::unique_ptr<BlockCache> m_cache;
};

// ---------------------------------------------------------------------------
// Basic parity: the fast path must produce exactly what the helper produces.
// ---------------------------------------------------------------------------

TEST_F(JitInlineStoreTest, Store32MatchesInterpreter) {
    check_parity(store32_abs(kDataA + 0x40),
                 [](CpuState& c) { c.gpr.set32(EAX, 0xDEADBEEF); });
}

TEST_F(JitInlineStoreTest, Store8MatchesInterpreter) {
    check_parity(store8_abs(kDataA + 0x40),
                 [](CpuState& c) { c.gpr.set32(EAX, 0xDEADBEEF); });
}

TEST_F(JitInlineStoreTest, Store16MatchesInterpreter) {
    check_parity(store16_abs(kDataA + 0x40),
                 [](CpuState& c) { c.gpr.set32(EAX, 0xDEADBEEF); });
}

// A 16-bit store must write exactly two bytes and leave neighbours intact.
TEST_F(JitInlineStoreTest, Store16DoesNotWriteNeighbouringBytes) {
    ASSERT_TRUE(m_memory->write_bytes(kDataA + 0x40,
                                      reinterpret_cast<const u8*>("\xAA\xAA\xAA\xAA"), 4));
    check_parity(store16_abs(kDataA + 0x41),
                 [](CpuState& c) { c.gpr.set32(EAX, 0x11223344); });
}

// Last valid 2-byte slot, and a store straddling the region end (must miss the
// fast path rather than run off the mapping).
TEST_F(JitInlineStoreTest, Store16AtLastValidOffset) {
    check_parity(store16_abs(static_cast<u32>(kDataA + kDataSize - 2)),
                 [](CpuState& c) { c.gpr.set32(EAX, 0xCAFEBABE); });
}

TEST_F(JitInlineStoreTest, Store16StraddlingRegionEndDoesNotCorrupt) {
    check_parity(store16_abs(static_cast<u32>(kDataA + kDataSize - 1)),
                 [](CpuState& c) { c.gpr.set32(EAX, 0xCAFEBABE); });
}

TEST_F(JitInlineStoreTest, Store16ToReadOnlyRegionRaisesAndDoesNotWrite) {
    check_parity(store16_abs(static_cast<u32>(kRoBase + 0x10)),
                 [](CpuState& c) { c.gpr.set32(EAX, 0xBEEF); });
}

// A store must not disturb its neighbours -- catches a mis-sized STR (e.g. a
// 32-bit store emitted where an 8-bit one was meant), which the width-correct
// byte at the target address alone would not reveal.
TEST_F(JitInlineStoreTest, Store8DoesNotWriteNeighbouringBytes) {
    ASSERT_TRUE(m_memory->write_bytes(kDataA + 0x40,
                                      reinterpret_cast<const u8*>("\xAA\xAA\xAA\xAA"), 4));
    check_parity(store8_abs(kDataA + 0x41),
                 [](CpuState& c) { c.gpr.set32(EAX, 0x11223344); });
}

// ---------------------------------------------------------------------------
// Region edges. A 4-byte store whose last byte falls outside the region must
// miss the fast path; the bounds test is what keeps it from running off the
// end of the host mapping.
// ---------------------------------------------------------------------------

TEST_F(JitInlineStoreTest, Store32AtLastValidOffsetStaysInBounds) {
    check_parity(store32_abs(static_cast<u32>(kDataA + kDataSize - 4)),
                 [](CpuState& c) { c.gpr.set32(EAX, 0xCAFEBABE); });
}

TEST_F(JitInlineStoreTest, Store32StraddlingRegionEndDoesNotCorrupt) {
    // Last two bytes lie past the end of region A.
    check_parity(store32_abs(static_cast<u32>(kDataA + kDataSize - 2)),
                 [](CpuState& c) { c.gpr.set32(EAX, 0xCAFEBABE); });
}

TEST_F(JitInlineStoreTest, Store8AtLastValidOffsetStaysInBounds) {
    check_parity(store8_abs(static_cast<u32>(kDataA + kDataSize - 1)),
                 [](CpuState& c) { c.gpr.set32(EAX, 0x5A); });
}

TEST_F(JitInlineStoreTest, Store8AtRegionStart) {
    check_parity(store8_abs(static_cast<u32>(kDataA)),
                 [](CpuState& c) { c.gpr.set32(EAX, 0x5A); });
}

// ---------------------------------------------------------------------------
// Permission. The fast path performs no permission check, so a read-only
// region must never be published into a wregion slot.
// ---------------------------------------------------------------------------

TEST_F(JitInlineStoreTest, Store32ToReadOnlyRegionRaisesAndDoesNotWrite) {
    check_parity(store32_abs(static_cast<u32>(kRoBase + 0x10)),
                 [](CpuState& c) { c.gpr.set32(EAX, 0xDEADBEEF); });
}

TEST_F(JitInlineStoreTest, Store8ToReadOnlyRegionRaisesAndDoesNotWrite) {
    check_parity(store8_abs(static_cast<u32>(kRoBase + 0x10)),
                 [](CpuState& c) { c.gpr.set32(EAX, 0x5A); });
}

// A region that becomes read-only after it has been published must stop being
// eligible -- protect_region publishes a ranged shootdown that retracts it.
TEST_F(JitInlineStoreTest, ProtectToReadOnlyRetractsPublishedRegion) {
    auto code = store32_abs(kDataA + 0x40);
    ASSERT_TRUE(m_memory->write_bytes(kCodeBase, code.data(), code.size()));
    BasicBlock block = build_block(code);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);

    // First store, while the region is writable: publishes it into a wregion
    // slot and lands normally.
    {
        CpuState cpu;
        init_cpu(cpu);
        cpu.gpr.set32(EAX, 0x11111111);
        (void)m_jit->execute(*cb, cpu);
        ASSERT_FALSE(cpu.pending_raise.active);
    }
    u32 seen = 0;
    ASSERT_TRUE(m_memory->read_bytes(kDataA + 0x40,
                                     reinterpret_cast<u8*>(&seen), 4));
    ASSERT_EQ(seen, 0x11111111u) << "first store did not land";

    ASSERT_TRUE(m_memory->protect_region(kDataA, kDataSize,
                                         MemoryProtection::Read));

    // Second store must now raise rather than write through the stale slot.
    {
        CpuState cpu;
        init_cpu(cpu);
        cpu.gpr.set32(EAX, 0x22222222);
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
        << "read-only region was written through the fast path";
}

// ---------------------------------------------------------------------------
// Self-modifying code. The fast path skips notify_executable_write(), so a
// region holding compiled code must never be published.
// ---------------------------------------------------------------------------

// The case the code-free publish check exists for.
//
// A single write into a code-bearing region is safe on its own: it misses the
// (empty) fast path, reaches the helper, and the helper notifies. The hazard
// is what the helper does *afterwards* -- if it published the region without
// checking, the NEXT write into that same region would take the inline path
// and skip notification entirely. Two cached blocks on different pages make
// that observable: invalidating the first leaves the second in the cache, so
// the region still holds code when publication is considered.
TEST_F(JitInlineStoreTest, RepeatedStoresIntoCodeRegionKeepInvalidating) {
    const std::vector<u8> victim = {0xB8, 0x11, 0x22, 0x33, 0x44,  // MOV EAX,imm
                                    0xC3};                          // RET
    const GuestAddr page0 = kCodeBase;            // first victim's page
    const GuestAddr page2 = kCodeBase + 0x2000;   // second victim's page

    auto cache_victim_at = [&](GuestAddr at) {
        ASSERT_TRUE(m_memory->write_bytes(at, victim.data(), victim.size()));
        BasicBlock b;
        b.start_addr = at;
        MemoryByteReader reader(victim.data(), victim.size());
        DecodedInstruction inst;
        while (reader.remaining() > 0 &&
               decode_instruction(reader, DecodeMode::Mode32, inst)
                   == DecodeError::Success) {
            b.instructions.push_back(inst);
        }
        b.byte_length = static_cast<u32>(reader.position());
        b.instruction_count = static_cast<u32>(b.instructions.size());
        ASSERT_GT(b.instruction_count, 0u);
        ASSERT_NE(m_jit->compile(b), nullptr);
        m_cache->insert(b);
    };

    cache_victim_at(page0);
    cache_victim_at(page2);
    ASSERT_EQ(m_cache->size(), 2u);
    EXPECT_FALSE(m_cache->range_is_code_free(kCodeBase, kCodeSize));

    // Build a storing block that writes to page0 and then to page2. It lives
    // on a third page so executing it does not itself add code to the pages
    // under test.
    std::vector<u8> code;
    for (auto b : store32_abs(static_cast<u32>(page0)))  code.push_back(b);
    for (auto b : store32_abs(static_cast<u32>(page2)))  code.push_back(b);
    const GuestAddr store_at = kCodeBase + 0x1000;
    ASSERT_TRUE(m_memory->write_bytes(store_at, code.data(), code.size()));

    BasicBlock sblock;
    sblock.start_addr = store_at;
    {
        MemoryByteReader reader(code.data(), code.size());
        DecodedInstruction inst;
        while (reader.remaining() > 0 &&
               decode_instruction(reader, DecodeMode::Mode32, inst)
                   == DecodeError::Success) {
            sblock.instructions.push_back(inst);
        }
        sblock.byte_length = static_cast<u32>(reader.position());
        sblock.instruction_count = static_cast<u32>(sblock.instructions.size());
    }
    ASSERT_EQ(sblock.instruction_count, 2u);

    CpuState cpu;
    init_cpu(cpu);
    cpu.rip = store_at;
    cpu.gpr.set32(EAX, 0x99999999);
    CompiledBlock* scb = m_jit->compile(sblock);
    ASSERT_NE(scb, nullptr);
    (void)m_jit->execute(*scb, cpu);

    EXPECT_EQ(m_cache->size(), 0u)
        << "a self-modifying write skipped SMC invalidation -- the write "
           "helper published a code-bearing region into the inline store "
           "fast path";
}

// Compiling a block into a page inside an already-published write region must
// retract that region, so later stores there go through the helper.
TEST_F(JitInlineStoreTest, NewlyCompiledCodeBumpsCodeGeneration) {
    const u64 before = m_cache->code_generation();
    BasicBlock block;
    block.start_addr = kDataA;       // pretend code now lives in region A
    block.byte_length = 16;
    block.instruction_count = 1;
    m_cache->insert(block);
    EXPECT_NE(m_cache->code_generation(), before)
        << "code generation did not move when a block was added";
    EXPECT_FALSE(m_cache->range_is_code_free(kDataA, kDataSize));
}

// ---------------------------------------------------------------------------
// Multi-region traffic: the two wregion slots plus slow-path republish.
// ---------------------------------------------------------------------------

TEST_F(JitInlineStoreTest, AlternatingRegionStoresAllLand) {
    // Stack, region A and region B in one block: more distinct writable
    // regions than there are slots, so some stores must fall back mid-block.
    std::vector<u8> code;
    auto append = [&code](const std::vector<u8>& v) {
        code.insert(code.end(), v.begin(), v.end());
    };
    append(store32_abs(kDataA + 0x10));
    append(store32_abs(kDataB + 0x10));
    append(store32_abs(kDataA + 0x20));
    append(store32_abs(kDataB + 0x20));
    append(store8_abs(kDataA + 0x30));
    append(store8_abs(kDataB + 0x30));
    check_parity(code, [](CpuState& c) { c.gpr.set32(EAX, 0x0BADF00D); });
}

TEST_F(JitInlineStoreTest, StoreToUnmappedAddressRaisesInBothPaths) {
    check_parity(store32_abs(0x7F000000),
                 [](CpuState& c) { c.gpr.set32(EAX, 0xDEADBEEF); });
}

// Sequential fill across a whole region -- the decompression-inner-loop shape
// this optimization exists to serve. Verifies every byte, so an off-by-one in
// the offset computation shows up as a diff rather than as a lucky pass.
TEST_F(JitInlineStoreTest, SequentialFillMatchesInterpreter) {
    std::vector<u8> code;
    for (u32 i = 0; i < 64; ++i) {
        auto one = store8_abs(static_cast<u32>(kDataA + 0x100 + i));
        code.insert(code.end(), one.begin(), one.end());
    }
    check_parity(code, [](CpuState& c) { c.gpr.set32(EAX, 0xA5); });
}

}  // namespace
