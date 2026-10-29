// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// The 32-bit guest-base (linear) STORE asks one per-host-page check byte
// (GuestMemory::linear_check_page_map()) instead of walking the code span, the
// 2 MiB chunk table and the 4 KiB code-page map and marking the write-watch
// dirty byte inline. A set byte means the host page holds translated code
// (kLinearCheckHoldsCode) or belongs to a live MEM_WRITE_WATCH allocation
// (kLinearCheckWatched); the store then takes the cold helper, which performs
// the write, the SMC notify AND the write-watch record.
//
// These tests pin the two obligations that the removed inline work used to
// carry -- self-modifying-code invalidation and write-watch dirty reporting
// (docs: getwritewatch_gc_landmine, locked_atomics_write_watch_landmine) --
// plus proof that the cheaper lowering is the one actually emitted.

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <memory>
#include <vector>

#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/linear_fault.hpp"
#include "core/cpu/decode/decoder.hpp"
#include "core/memory/write_watch_traps.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

constexpr u8 EAX = static_cast<u8>(RegIndex::RAX);
constexpr u8 EBX = static_cast<u8>(RegIndex::RBX);
constexpr u8 ECX = static_cast<u8>(RegIndex::RCX);
constexpr u8 EDX = static_cast<u8>(RegIndex::RDX);
constexpr u8 ESP = static_cast<u8>(RegIndex::RSP);

// 16 KiB-aligned: macOS ARM64 host pages are 16 KiB and MAP_FIXED into the
// reservation requires that alignment.
constexpr GuestAddr kCode = 0x10000;
constexpr GuestAddr kData = 0x00400000;
constexpr usize     kDataSize = 0x4000;
// A 32-bit guest address must always index inside the check map, so the
// reservation has to span the whole 4 GiB address space.
constexpr usize     kReservation = usize{4} * 1024 * 1024 * 1024;

class JitLinearStoreCheckByteTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        setenv("VITREO_JIT_LINEAR", "1", 1);
        ASSERT_TRUE(install_linear_fault_handler());
    }

    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->enable_linear_reservation(kReservation));
        ASSERT_TRUE(register_linear_reservation(m_memory->linear_base(),
                                                kReservation));
        // Without the check map the legacy multi-map probe is emitted and none
        // of this is under test.
        ASSERT_NE(m_memory->linear_check_page_map(), nullptr);
        ASSERT_TRUE(m_memory->map_region(kCode, 0x4000,
                                         MemoryProtection::ReadWrite));
        ASSERT_TRUE(m_memory->map_region(kData, kDataSize,
                                         MemoryProtection::ReadWrite));
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_cache = std::make_unique<BlockCache>();
        m_jit->set_block_cache(m_cache.get());
    }

    void TearDown() override {
        if (m_memory) unregister_linear_reservation(m_memory->linear_base());
    }

    static BasicBlock decode_at(GuestAddr at, const std::vector<u8>& code) {
        BasicBlock b;
        b.start_addr = at;
        MemoryByteReader r(code.data(), code.size());
        while (r.remaining() > 0) {
            DecodedInstruction i;
            if (decode_instruction(r, DecodeMode::Mode32, i) !=
                DecodeError::Success) {
                break;
            }
            b.instructions.push_back(i);
        }
        b.byte_length = static_cast<u32>(r.position());
        b.instruction_count = static_cast<u32>(b.instructions.size());
        return b;
    }

    BasicBlock build_block(const std::vector<u8>& code) {
        return decode_at(kCode, code);
    }

    void init_cpu(CpuState& c) {
        c.init_protected_mode();
        c.gpr.set32(ESP, 0x10000);
        c.rip = kCode;
    }

    struct WatchScope {
        explicit WatchScope(GuestAddr base, usize size) : m_base(base) {
            write_watch::register_region(base, size, m_dirty.data(),
                                         m_armed.data(), /*executable=*/false);
        }
        ~WatchScope() { write_watch::unregister_region(m_base); }
        // GetWriteWatch reports a page when EITHER channel has it: the
        // per-slot bits that the recording helper sets, or the linear page map
        // that a direct store marks (see kernel32's GetWriteWatch). A test
        // asserting only one of them would fail on a lowering that is
        // perfectly correct for the guest.
        [[nodiscard]] bool reported(usize guest_page) const {
            return write_watch::test_bit(m_dirty.data(), guest_page) ||
                   write_watch::test_linear_page(
                       m_base + guest_page * write_watch::kGuestPage);
        }
        GuestAddr m_base;
        std::array<u8, 8> m_dirty{};
        std::array<u8, 8> m_armed{};
    };

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
    std::unique_ptr<BlockCache> m_cache;
};

// Everything below tests behaviour that the legacy multi-map probe also
// satisfies, so without this the whole group could pass on a silently disabled
// fast path.
TEST_F(JitLinearStoreCheckByteTest, CheckByteLoweringIsEmittedAndSmaller) {
    const std::vector<u8> code = {0x89, 0x03};  // MOV [EBX], EAX
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));

    // The mode is read once, by the constructor, so the two lowerings can be
    // compared side by side against the same GuestMemory.
    auto compile_with = [&](const char* checkmap) -> u32 {
        setenv("VITREO_JIT_LINEAR32_CHECKMAP", checkmap, 1);
        BlockCache cache;
        JitCompiler jit(*m_memory);
        jit.set_block_cache(&cache);
        BasicBlock block = build_block(code);
        CompiledBlock* cb = jit.compile(block);
        return cb == nullptr ? 0u : cb->code_size;
    };
    const u32 legacy = compile_with("0");
    const u32 checked = compile_with("1");
    unsetenv("VITREO_JIT_LINEAR32_CHECKMAP");

    ASSERT_NE(legacy, 0u);
    ASSERT_NE(checked, 0u);
    EXPECT_LT(checked, legacy)
        << "the per-host-page check byte did not replace the span/chunk/page "
           "probe (checked=" << checked << " legacy=" << legacy << ")";
}

// An under-reported GetWriteWatch dirty page is a silent lie that makes a guest
// collector free live objects. The check byte removed the inline mark every
// store used to pay, so the obligation now rests entirely on
// kLinearCheckWatched routing a watched page to the recording helper.
TEST_F(JitLinearStoreCheckByteTest, WriteWatchSeesDirectStore) {
    WatchScope watch(kData, kDataSize);

    const std::vector<u8> code = {0x89, 0x03};  // MOV [EBX], EAX
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);
    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(EBX, static_cast<u32>(kData + 0x1000));  // 2nd guest page
    cpu.gpr.set32(EAX, 0xCAFEBABE);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    (void)m_jit->execute(*cb, cpu);

    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_TRUE(watch.reported(1)) << "a direct 32-bit store was reported clean";
    EXPECT_EQ(m_memory->read_u32(kData + 0x1000).value_or(0), 0xCAFEBABEu);
}

// A store straddling two guest pages must report BOTH dirty.
TEST_F(JitLinearStoreCheckByteTest, WriteWatchSeesStraddlingStore) {
    WatchScope watch(kData, kDataSize);

    const std::vector<u8> code = {0x89, 0x03};  // MOV [EBX], EAX
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);
    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(EBX, static_cast<u32>(kData + 0x1000 - 2));
    cpu.gpr.set32(EAX, 0x11223344);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    (void)m_jit->execute(*cb, cpu);

    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_TRUE(watch.reported(0));
    EXPECT_TRUE(watch.reported(1))
        << "the far half of a straddling store was reported clean";
    EXPECT_EQ(m_memory->read_u32(kData + 0x1000 - 2).value_or(0), 0x11223344u);
}

// A store crossing a 16 KiB HOST page boundary has two check bytes, and either
// one being set must route it. Watching only the upper half of a single region
// proves the far endpoint is really consulted rather than assumed to match the
// first. One region, because an access spanning two separate allocations is a
// fault under the interpreter too.
TEST_F(JitLinearStoreCheckByteTest, WriteWatchSeesStoreCrossingIntoWatchedPage) {
    constexpr GuestAddr kBig = 0x00800000;
    ASSERT_TRUE(m_memory->map_region(kBig, 0x8000,
                                     MemoryProtection::ReadWrite));
    WatchScope watch(kBig + 0x4000, 0x4000);   // upper host page only

    const std::vector<u8> code = {0x89, 0x03};  // MOV [EBX], EAX
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);
    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(EBX, static_cast<u32>(kBig + 0x4000 - 2));  // 2 bytes each
    cpu.gpr.set32(EAX, 0xA5A5F0F0);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    (void)m_jit->execute(*cb, cpu);

    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_TRUE(watch.reported(0))
        << "a store entering a watched host page from the page below was "
           "reported clean";
    EXPECT_EQ(m_memory->read_u32(kBig + 0x4000 - 2).value_or(0), 0xA5A5F0F0u);
}

// Exact-when-cheap: a store that ends on the last bytes of a host page must
// NOT consult the neighbour check byte. Watching only the next host page used
// to send every such store through the cold helper under the LDRH probe
// (neighbour-only false positive). The write must still land, and the watched
// neighbour must stay clean.
TEST_F(JitLinearStoreCheckByteTest, StoreBesideWatchedNeighbourStaysInline) {
    constexpr GuestAddr kBig = 0x00880000;
    ASSERT_TRUE(m_memory->map_region(kBig, 0x8000,
                                     MemoryProtection::ReadWrite));
    WatchScope watch(kBig + 0x4000, 0x4000);  // upper host page only

    const std::vector<u8> code = {0x89, 0x03};  // MOV [EBX], EAX
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);
    CpuState cpu;
    init_cpu(cpu);
    // Last 4 bytes of the lower host page — touches only that page.
    cpu.gpr.set32(EBX, static_cast<u32>(kBig + 0x4000 - 4));
    cpu.gpr.set32(EAX, 0xAABBCCDD);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    (void)m_jit->execute(*cb, cpu);

    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(m_memory->read_u32(kBig + 0x4000 - 4).value_or(0), 0xAABBCCDDu);
    EXPECT_FALSE(watch.reported(0))
        << "a non-straddling store beside a watched neighbour marked it dirty";
}

// A LOCK-prefixed read-modify-write is a store too.
TEST_F(JitLinearStoreCheckByteTest, WriteWatchSeesLockedRmw) {
    WatchScope watch(kData, kDataSize);

    const std::vector<u8> code = {0xF0, 0xFF, 0x03};  // LOCK INC dword [EBX]
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);
    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(EBX, static_cast<u32>(kData + 0x1000));
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    (void)m_jit->execute(*cb, cpu);

    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_TRUE(watch.reported(1)) << "a LOCK-prefixed RMW was reported clean";
    EXPECT_EQ(m_memory->read_u32(kData + 0x1000).value_or(0), 1u);
}

// A store onto a page holding a compiled block must still invalidate it: the
// check byte's kLinearCheckHoldsCode is now the only thing that routes the
// store to the helper that notifies.
TEST_F(JitLinearStoreCheckByteTest, StoreOntoCompiledPageInvalidatesBlock) {
    const std::vector<u8> victim = {0xB8, 0x11, 0x22, 0x33, 0x44, 0xC3};
    ASSERT_TRUE(m_memory->write_bytes(kCode, victim.data(), victim.size()));
    BasicBlock vblock = build_block(victim);
    ASSERT_NE(m_jit->compile(vblock), nullptr);
    m_cache->insert(vblock);
    ASSERT_EQ(m_cache->size(), 1u);

    const GuestAddr store_site = kCode + 0x800;
    const std::vector<u8> code = {0x89, 0x03};  // MOV [EBX], EAX
    ASSERT_TRUE(m_memory->write_bytes(store_site, code.data(), code.size()));
    BasicBlock sblock = decode_at(store_site, code);
    CpuState cpu;
    init_cpu(cpu);
    cpu.rip = store_site;
    cpu.gpr.set32(EBX, static_cast<u32>(kCode));   // overwrite the victim
    cpu.gpr.set32(EAX, 0x90909090u);
    CompiledBlock* scb = m_jit->compile(sblock);
    ASSERT_NE(scb, nullptr);
    (void)m_jit->execute(*scb, cpu);

    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(m_memory->read_u32(kCode).value_or(0), 0x90909090u);
    EXPECT_TRUE(m_jit->synchronize_executable_writes());
    EXPECT_EQ(m_cache->size(), 0u)
        << "a store onto a compiled page did not invalidate its translation";
}

// The check byte is per 16 KiB HOST page, so a store on the guest page NEXT to
// compiled code shares its byte and takes the helper. That is conservative, not
// wrong: the write must still land and must not invalidate the neighbour.
TEST_F(JitLinearStoreCheckByteTest, StoreBesideCodeInSameHostPageStillLands) {
    const std::vector<u8> victim = {0xB8, 0x11, 0x22, 0x33, 0x44, 0xC3};
    ASSERT_TRUE(m_memory->write_bytes(kCode, victim.data(), victim.size()));
    BasicBlock vblock = build_block(victim);
    ASSERT_NE(m_jit->compile(vblock), nullptr);
    m_cache->insert(vblock);
    ASSERT_EQ(m_cache->size(), 1u);

    const GuestAddr store_site = kCode + 0x800;
    const GuestAddr target = kCode + 0x1000;  // next guest page, same host page
    const std::vector<u8> code = {0x89, 0x03};  // MOV [EBX], EAX
    ASSERT_TRUE(m_memory->write_bytes(store_site, code.data(), code.size()));
    BasicBlock sblock = decode_at(store_site, code);
    CpuState cpu;
    init_cpu(cpu);
    cpu.rip = store_site;
    cpu.gpr.set32(EBX, static_cast<u32>(target));
    cpu.gpr.set32(EAX, 0x0D15EA5Eu);
    CompiledBlock* scb = m_jit->compile(sblock);
    ASSERT_NE(scb, nullptr);
    (void)m_jit->execute(*scb, cpu);

    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(m_memory->read_u32(target).value_or(0), 0x0D15EA5Eu);
    // False: nothing to synchronize. The helper notifies from the 4 KiB page
    // that actually holds code, so the coarser check byte costs a helper call
    // and nothing else.
    EXPECT_FALSE(m_jit->synchronize_executable_writes());
    EXPECT_EQ(m_cache->size(), 1u)
        << "a store on a page holding no code invalidated a neighbour";
}

// The check-byte sequence is NZCV-clean, so the guest-flag save moved into its
// cold stub. A store standing between an x86 flag producer and its consumer
// must still branch on the producer's flags -- on the inline path AND on the
// helper path (a store onto a page that holds compiled code).
TEST_F(JitLinearStoreCheckByteTest, FlagsSurviveStoreBetweenProducerAndConsumer) {
    // CMP EAX,ECX ; MOV [EBX],EDX ; SETB AL
    const std::vector<u8> code = {0x39, 0xC8, 0x89, 0x13, 0x0F, 0x92, 0xC0};
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);

    for (const GuestAddr target : {kData + 0x40, kCode + 0x400}) {
        CpuState cpu;
        init_cpu(cpu);
        cpu.gpr.set32(EAX, 1);
        cpu.gpr.set32(ECX, 2);      // 1 < 2 unsigned: CF set, so SETB -> 1
        cpu.gpr.set32(EBX, static_cast<u32>(target));
        cpu.gpr.set32(EDX, 0x5A5A5A5Au);
        (void)m_jit->execute(*cb, cpu);
        EXPECT_FALSE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.gpr.get32(EAX) & 0xFFu, 1u)
            << "flags lost across a store to 0x" << std::hex
            << static_cast<u64>(target);
        EXPECT_EQ(m_memory->read_u32(target).value_or(0), 0x5A5A5A5Au);
    }
}

// The 32-bit shape of JitLinear64Test.MemoryAddImmediateAboveTheWindowSets-
// TheResultsFlags (Spotify's renderer, 2026-10-22): a memory-destination ADD,
// a JE after it, and a store that takes the cold stub's write helper, which
// clobbers the caller-saved registers. 32-bit flag liveness publishes this
// ADD's record inline today, so the skipped-record replay that failed in
// 64-bit code does not arise here; this guards both edges if it ever does.
// ADD sets ZF exactly when the sum is zero (Intel SDM Vol. 2A, ADD): a
// nonzero sum falls through to the JMP, a zero sum takes JE with the ADD's
// flags. The target shares a host page with the compiled code.
TEST_F(JitLinearStoreCheckByteTest, MemoryAddFlagsSurviveItsOwnHelperStore) {
    const std::vector<u8> code = {
        0x83, 0x03, 0xF8,              // add dword [ebx],-8
        0x74, 0x19,                    // je +0x19
        0x85, 0xF6,                    // test esi,esi
        0x74, 0x1A,                    // je +0x1a
        0x89, 0xF1,                    // mov ecx,esi
        0x83, 0xC4, 0x20,              // add esp,0x20
        0x5B, 0x5F, 0x5E,              // pop ebx; pop edi; pop esi
        0xE9, 0x00, 0x10, 0x00, 0x00,  // jmp +0x1000
    };
    ASSERT_EQ(code.size(), 22u);
    constexpr GuestAddr kJeTarget = kCode + 0x1E;
    constexpr GuestAddr kJmpTarget = kCode + 22 + 0x1000;
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);
    ASSERT_EQ(block.instruction_count, 10u);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    // The next guest page in the code's 16 KiB host page: the check byte
    // sends the store to the cold stub's helper, and no code is overwritten.
    const GuestAddr target = kCode + 0x1000;
    const GuestAddr stack = kData + 0x1000;
    for (const u32 before : {0x100u, 8u}) {
        SCOPED_TRACE(::testing::Message() << "[ebx] = " << before);
        ASSERT_TRUE(m_memory->write_u32(target, before));
        ASSERT_TRUE(m_memory->write_u32(stack + 0x20, 0x1111));  // ebx
        ASSERT_TRUE(m_memory->write_u32(stack + 0x24, 0x2222));  // edi
        ASSERT_TRUE(m_memory->write_u32(stack + 0x28, 0x3333));  // esi
        CpuState cpu;
        init_cpu(cpu);
        cpu.gpr.set32(EBX, static_cast<u32>(target));
        cpu.gpr.set32(ESP, static_cast<u32>(stack));
        cpu.gpr.set32(6, 0x00584190u);  // ESI
        cpu.flags.rflags = before == 8u ? 0x202u : 0x242u;  // ZF opposite the answer
        (void)m_jit->execute(*cb, cpu);
        ASSERT_FALSE(cpu.pending_raise.active);
        EXPECT_EQ(m_memory->read_u32(target).value_or(0), before - 8u);
        if (before == 8u) {
            EXPECT_EQ(cpu.rip, kJeTarget) << "JE not taken on a zero sum";
            // The taken edge leaves with the ADD's flags live: 8 + 0xFFFFFFF8
            // is 0 with a carry out of bit 31 and of bit 3, even parity.
            cpu.materialize_lazy_flags();
            EXPECT_EQ(cpu.flags.rflags & 0x8D5u, 0x055u)
                << "ZF CF AF PF, from the ADD's record on the JE's taken edge";
            continue;
        }
        EXPECT_EQ(cpu.rip, kJmpTarget) << "JE taken on a nonzero sum";
        EXPECT_EQ(cpu.gpr.get32(1), 0x00584190u);  // ECX
        EXPECT_EQ(cpu.gpr.get32(EBX), 0x1111u);
        EXPECT_EQ(cpu.gpr.get32(7), 0x2222u);      // EDI
        EXPECT_EQ(cpu.gpr.get32(6), 0x3333u);      // ESI
        EXPECT_EQ(cpu.gpr.get32(ESP), static_cast<u32>(stack) + 0x2Cu);
    }
}

// A CALL through a register must reach the callee even when the return-address
// push takes the cold stub.
//
// The check byte has HOST page granularity (16 KiB), so a guest stack sharing
// a host page with a MEM_WRITE_WATCH allocation sends EVERY push through the
// stub -- and the stub calls the write helper, across which every caller-saved
// register dies (`emit_helper_blr` reloads only the pins and the linear base).
// The stub additionally uses W0/W1 as its own flag-replay temporaries. So a
// lowering that parks the branch target in a scratch across the push loses it
// and jumps to a leftover value, which is a stack address -- the guest lands on
// its own stack. The target belongs in REG_CALL_TMP (X21), inside AAPCS64's
// callee-saved X19-X28.
//
// The Sims 3 hit this on essentially every call: Mono maps its code heap
// directly above its thread stack.
TEST_F(JitLinearStoreCheckByteTest, IndirectCallTargetSurvivesHelperStackPush) {
    constexpr GuestAddr kCallee = kData + 0x800;
    constexpr GuestAddr kCallSite = kCode + 0x800;
    const std::vector<u8> code = {0xFF, 0xD3};  // CALL EBX
    ASSERT_TRUE(m_memory->write_bytes(kCallSite, code.data(), code.size()));
    BasicBlock block = decode_at(kCallSite, code);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);

    // Control: an ordinary stack, which takes the inline store.
    CpuState quiet;
    init_cpu(quiet);
    quiet.rip = kCallSite;
    quiet.gpr.set32(ESP, static_cast<u32>(kData + 0x3000));
    quiet.gpr.set32(EBX, static_cast<u32>(kCallee));
    (void)m_jit->execute(*cb, quiet);
    ASSERT_FALSE(quiet.pending_raise.active);
    ASSERT_EQ(quiet.rip, kCallee) << "indirect call is broken on the fast path";

    // Now put the guest stack on a host page that holds translated code, so
    // the push is forced through the stub's HELPER arm.
    //
    // A merely write-watched page is not enough: the stub serves a byte of
    // exactly kLinearCheckWatched on its warm arm, storing inline and
    // returning without a call, so nothing is clobbered and this test would
    // pass against the bug it exists to catch. kLinearCheckHoldsCode is what
    // reaches the helper -- and that is the real configuration, because a
    // guest that maps its code heap next to its thread stack (Mono, in The
    // Sims 3) marks the shared 16 KiB host page exactly this way.
    const std::vector<u8> victim = {0xB8, 0x11, 0x22, 0x33, 0x44, 0xC3};
    ASSERT_TRUE(m_memory->write_bytes(kCode, victim.data(), victim.size()));
    BasicBlock vblock = build_block(victim);
    ASSERT_NE(m_jit->compile(vblock), nullptr);
    m_cache->insert(vblock);

    const u32 esp = static_cast<u32>(kCode + 0x2000);  // same host page as kCode
    const u8* check = m_memory->linear_check_page_map();
    ASSERT_NE(check, nullptr);
    ASSERT_TRUE(check[(esp - 4) >> 14] & kLinearCheckHoldsCode)
        << "the stack page is not marked, so the push stays on the fast path "
           "and the clobber this test exists to catch is never exercised";

    CpuState cpu;
    init_cpu(cpu);
    cpu.rip = kCallSite;
    cpu.gpr.set32(ESP, esp);
    cpu.gpr.set32(EBX, static_cast<u32>(kCallee));
    (void)m_jit->execute(*cb, cpu);

    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.rip, kCallee)
        << "branch target lost across the return-address store; jumped to 0x"
        << std::hex << static_cast<u64>(cpu.rip);
    EXPECT_EQ(cpu.gpr.get32(ESP), esp - 4);
    EXPECT_EQ(m_memory->read_u32(esp - 4).value_or(0),
              static_cast<u32>(kCallSite + code.size()));
}

// A store to an address with no mapping must still raise a guest access
// violation, through the recoverable fault handler rather than any check.
TEST_F(JitLinearStoreCheckByteTest, StoreToUnmappedStillRaises) {
    constexpr GuestAddr kUnmapped = 0x30000000;
    const std::vector<u8> code = {0x89, 0x03};  // MOV [EBX], EAX
    ASSERT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
    BasicBlock block = build_block(code);
    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set32(EBX, static_cast<u32>(kUnmapped));
    cpu.gpr.set32(EAX, 0xDEADBEEF);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    (void)m_jit->execute(*cb, cpu);

    EXPECT_TRUE(cpu.pending_raise.active)
        << "a store to unmapped memory did not raise";
}


// Direct contract test for the recording helper the cold stub calls. An access
// may start outside every watched region and end inside one; reporting that
// page clean is the under-report that frees live objects.
TEST(WriteWatchRecordStore, StoreEndingInsideWatchedRegionMarksItsFirstPage) {
    constexpr GuestAddr kBase = 0x00900000;
    std::array<u8, 8> dirty{};
    std::array<u8, 8> armed{};
    write_watch::register_region(kBase, 0x4000, dirty.data(), armed.data(),
                                 /*executable=*/false);
    write_watch::record_store(kBase - 2, 4);
    const bool marked = write_watch::test_bit(dirty.data(), 0);
    write_watch::unregister_region(kBase);
    EXPECT_TRUE(marked)
        << "a store ending inside a watched region left its page clean";
}

}  // namespace
