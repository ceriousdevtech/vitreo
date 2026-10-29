/// @file test_block_cache.cpp
/// @brief Tests for JIT basic block cache (CPU-035)

#include <gtest/gtest.h>
#include <core/cpu/jit/block_cache.hpp>
#include <core/cpu/jit/jit_compiler.hpp>
#include <core/cpu/decode/decoder.hpp>
#include <core/cpu/decode/guest_mapped_reader.hpp>
#include <core/memory/guest_memory.hpp>
#include <array>
#include <memory>
#include <type_traits>
#include <vector>

using namespace vitreo::cpu::jit;
using namespace vitreo::cpu;
using namespace vitreo;

namespace {

/// See BlockCache::l1_alias_after: an address that deliberately collides with
/// `base` in L1, derived from the index formula rather than assuming a stride.
GuestAddr l1_alias_of(GuestAddr base, GuestAddr min_distance) {
    return BlockCache::l1_alias_after(base, min_distance);
}

/// BlockCache::sticky_index is private; mirror it (see block_cache.hpp).
usize sticky_index_of(GuestAddr a) {
    return static_cast<usize>(((a >> BlockCache::l1_index_fold()) ^ (a >> 32)) &
                              255u);
}

BasicBlock make_block(GuestAddr addr, u32 byte_len = 10, u32 instr_count = 3) {
    BasicBlock b;
    b.start_addr = addr;
    b.byte_length = byte_len;
    b.instruction_count = instr_count;
    b.ends_with_branch = true;
    b.execution_count = 0;
    return b;
}

class BlockCacheTest : public ::testing::Test {
protected:
    // Declaration order is load-bearing. ~BlockCache calls back into
    // GuestMemory::release_jit_code_page for any block still holding a code
    // page, so the memory must outlive the cache. Members are destroyed in
    // reverse declaration order, which makes this ordering (memory first,
    // cache second) the one that matches production, where a single
    // GuestMemory outlives every cache built against it.
    //
    // A test-local GuestMemory with the fixture-member cache inverts that and
    // aborts the whole binary on a destroyed std::mutex rather than failing
    // one test.
    GuestMemory memory;
    BlockCache cache{64};
};

// 1. EmptyLookupReturnsNull
TEST_F(BlockCacheTest, EmptyLookupReturnsNull) {
    EXPECT_EQ(cache.lookup(0x1000), nullptr);
    EXPECT_EQ(cache.size(), 0u);
}

TEST_F(BlockCacheTest, InvalidationSeparatesDecodedAndCompiledEntries) {
    cache.insert(make_block(0x1000));
    auto native = make_block(0x1020);
    u32 native_storage = 0;
    native.compiled_code = &native_storage; // identity only; never executed
    cache.insert(std::move(native));
    std::vector<GuestAddr> all, compiled;
    EXPECT_EQ(cache.invalidate_if_cached_page(0x1000, 0x40, &all, &compiled), 2u);
    EXPECT_EQ(all.size(), 2u);
    ASSERT_EQ(compiled.size(), 1u);
    EXPECT_EQ(compiled[0], 0x1020u);
    EXPECT_EQ(cache.lookup(0x1000), nullptr);
    EXPECT_EQ(cache.lookup(0x1020), nullptr);
}

TEST_F(BlockCacheTest, DensePageRemovalPreservesCoverageAndCrossPageOwnership) {
    for (const bool long_mode : {false, true}) {
        const GuestAddr base = long_mode ? 0x100001000ull : 0x1000ull;
        BlockCache dense{1024};
        for (unsigned i = 0; i < 512; ++i) {
            auto block = make_block(base + i * 4, 16);
            block.is_64bit = long_mode;
            dense.insert(std::move(block));
        }
        // Insertion order is used by find_covering. Removing a single start
        // must preserve the other starts, including overlapping spans.
        ASSERT_NE(dense.find_covering(base + 10), nullptr);
        EXPECT_EQ(dense.find_covering(base + 10)->start_addr, base);
        ASSERT_TRUE(dense.invalidate(base));
        ASSERT_NE(dense.find_covering(base + 10), nullptr);
        EXPECT_EQ(dense.find_covering(base + 10)->start_addr, base + 4);
        ASSERT_TRUE(dense.invalidate(base + 256 * 4));
        ASSERT_TRUE(dense.invalidate(base + 511 * 4));
        for (unsigned i = 1; i < 511; ++i) {
            if (i != 256) EXPECT_NE(dense.lookup_fast(base + i * 4), nullptr);
        }
        auto crossing = make_block(base + 0xff8, 24);
        crossing.is_64bit = long_mode;
        dense.insert(std::move(crossing));
        EXPECT_EQ(dense.invalidate_if_cached_page(base + 0x1000, 1), 1u);
        EXPECT_EQ(dense.lookup_fast(base + 0xff8), nullptr);
        EXPECT_EQ(dense.invalidate_if_cached_page(base + 0x1000, 1), 0u);
        EXPECT_EQ(dense.invalidate_if_cached_page(base, 0x1000), 509u);
        EXPECT_EQ(dense.size(), 0u);
        EXPECT_EQ(dense.find_covering(base + 10), nullptr);
    }
}

// 2. InsertAndLookupSucceeds
TEST_F(BlockCacheTest, InsertAndLookupSucceeds) {
    cache.insert(make_block(0x1000));
    const BasicBlock* b = cache.lookup(0x1000);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->start_addr, 0x1000u);
}

TEST(BlockPublication, FullyNativeBlockReleasesDecodedInstructions) {
    BasicBlock block = make_block(0x1000, 4, 2);
    block.instructions.resize(2);
    ASSERT_GT(block.instructions.capacity(), 0u);

    CompiledBlock compiled{};
    compiled.native_insns = 2;
    compiled.hot_dump_variant = 11;
    publish_compiled(block, compiled);

    EXPECT_TRUE(block.instructions.empty());
    EXPECT_EQ(block.instructions.capacity(), 0u);
    EXPECT_EQ(block.instruction_count, 2u);
    EXPECT_EQ(block.compiled_hot_dump_variant, 11u);
}

TEST(BlockPublication, DelegatedBlockRetainsDecodedInstructions) {
    // A delegate stencil bakes `&instructions[i]`, so the array has to outlive
    // publication. `native_insns != instruction_count` used to stand in for
    // that, but publish_compiled now asks the exact question the lifetime
    // depends on -- whether the compiled record itself carries the array --
    // and a record that answers no releases it -- which is exactly what
    // FullyNativeBlockReleasesDecodedInstructions above asserts. Set the
    // delegate array, not the old proxy, or this test asserts the opposite of
    // the shipped contract.
    BasicBlock block = make_block(0x1000, 4, 2);
    block.instructions.resize(2);

    CompiledBlock compiled{};
    compiled.native_insns = 1;
    compiled.delegate_insns = block.instructions.share();
    publish_compiled(block, compiled);

    EXPECT_EQ(block.instructions.size(), 2u);
}

TEST(BlockPublication, ReconstructsEveryCompiledBlockField) {
    std::array<u8, 64> code{};
    BasicBlock block = make_block(0x123456789ABCull, 0x37, 2);
    block.instructions.resize(2);
    const std::weak_ptr<const DecodedInsns::Storage> decoded =
        block.instructions.share();
    block.compiled_code = code.data();
    block.compiled_link_code = code.data() + 20;
    block.compiled_call_code = code.data() + 28;
    block.compiled_code_size = 60;
    block.compiled_arena_generation = 0x1122334455667788ull;
    block.compiled_native_insns = 1;
    block.compiled_hot_dump_variant = 7;
    block.compiled_requires_protection_lease = false;
    block.compiled_pin_layout = 0x123456789ABull;

    CompiledBlock compiled = compiled_from_cache(block);

    EXPECT_EQ(compiled.code, block.compiled_code);
    EXPECT_EQ(compiled.code_size, block.compiled_code_size);
    EXPECT_EQ(compiled.guest_addr, block.start_addr);
    EXPECT_EQ(compiled.guest_size, block.byte_length);
    EXPECT_EQ(compiled.arena_generation, block.compiled_arena_generation);
    EXPECT_EQ(compiled.native_insns, block.compiled_native_insns);
    EXPECT_EQ(compiled.hot_dump_variant, block.compiled_hot_dump_variant);
    EXPECT_EQ(compiled.requires_protection_lease,
              block.compiled_requires_protection_lease);
    EXPECT_EQ(compiled.pin_layout, block.compiled_pin_layout);
    EXPECT_EQ(compiled.link_code_offset, 20u);
    EXPECT_EQ(compiled.call_code_offset, 28u);
    EXPECT_EQ(compiled.delegate_insns, decoded.lock());
    EXPECT_EQ(compiled.decoded_owner, nullptr);
    EXPECT_EQ(compiled.link_entry(), block.compiled_link_code);
    EXPECT_EQ(compiled.call_entry(), block.compiled_call_code);

    block.instructions.reset();
    EXPECT_TRUE(block.instructions.empty());
    ASSERT_NE(compiled.delegate_insns, nullptr);
    EXPECT_EQ(compiled.delegate_insns->size(), 2u);
    EXPECT_FALSE(decoded.expired());
}

TEST(BlockPublication, MalformedCachedLinkFailsClosed) {
    std::array<u8, 64> code{};
    BasicBlock block = make_block(0x4000, 4, 1);
    block.compiled_code = code.data();
    block.compiled_code_size = 32;

    block.compiled_link_code = nullptr;
    EXPECT_EQ(compiled_from_cache(block).link_entry(), nullptr);

    block.compiled_link_code = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(code.data()) - sizeof(u32));
    EXPECT_EQ(compiled_from_cache(block).link_entry(), nullptr);

    block.compiled_link_code = code.data() + block.compiled_code_size;
    EXPECT_EQ(compiled_from_cache(block).link_entry(), nullptr);

    block.compiled_link_code = code.data() + 6;
    EXPECT_EQ(compiled_from_cache(block).link_entry(), nullptr);
}

TEST(BlockPublication, MalformedCachedCallEntryFailsClosed) {
    std::array<u8, 64> code{};
    BasicBlock block = make_block(0x4000, 4, 1);
    block.compiled_code = code.data();
    block.compiled_code_size = 32;

    block.compiled_call_code = nullptr;
    EXPECT_EQ(compiled_from_cache(block).call_entry(), nullptr);

    block.compiled_call_code = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(code.data()) - sizeof(u32));
    EXPECT_EQ(compiled_from_cache(block).call_entry(), nullptr);

    block.compiled_call_code = code.data() + block.compiled_code_size;
    EXPECT_EQ(compiled_from_cache(block).call_entry(), nullptr);

    block.compiled_call_code = code.data() + 6;
    EXPECT_EQ(compiled_from_cache(block).call_entry(), nullptr);

    block.compiled_call_code = code.data() + 16;
    block.compiled_requires_protection_lease = true;
    EXPECT_EQ(compiled_from_cache(block).call_entry(), nullptr);
}

// 3. LookupReturnsCorrectData
TEST_F(BlockCacheTest, LookupReturnsCorrectData) {
    cache.insert(make_block(0x2000, 20, 5));
    const BasicBlock* b = cache.lookup(0x2000);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->start_addr, 0x2000u);
    EXPECT_EQ(b->byte_length, 20u);
    EXPECT_EQ(b->instruction_count, 5u);
    EXPECT_TRUE(b->ends_with_branch);
    EXPECT_EQ(b->execution_count, 0u);
}

// 4. InsertMultipleLookupEach
TEST_F(BlockCacheTest, InsertMultipleLookupEach) {
    cache.insert(make_block(0x1000));
    cache.insert(make_block(0x2000));
    cache.insert(make_block(0x3000));

    EXPECT_NE(cache.lookup(0x1000), nullptr);
    EXPECT_NE(cache.lookup(0x2000), nullptr);
    EXPECT_NE(cache.lookup(0x3000), nullptr);
    EXPECT_EQ(cache.size(), 3u);
}

// 5. EvictsLRUWhenFull
TEST_F(BlockCacheTest, EvictsLRUWhenFull) {
    BlockCache small_cache{4};
    small_cache.insert(make_block(0x1000));  // will be evicted
    small_cache.insert(make_block(0x2000));
    small_cache.insert(make_block(0x3000));
    small_cache.insert(make_block(0x4000));
    EXPECT_EQ(small_cache.size(), 4u);

    // Insert 5th block — should evict 0x1000 (LRU)
    small_cache.insert(make_block(0x5000));
    EXPECT_EQ(small_cache.size(), 4u);
    EXPECT_EQ(small_cache.lookup(0x1000), nullptr);
    EXPECT_NE(small_cache.lookup(0x5000), nullptr);
}

// 6. InvalidateRemovesBlock
TEST_F(BlockCacheTest, FastHitsProtectHotBlockFromColdInsertions) {
    BlockCache small_cache{3};
    small_cache.insert(make_block(0x1000));
    small_cache.insert(make_block(0x1040));
    small_cache.insert(make_block(0x1080));
    for (unsigned i = 0; i < 8; ++i) {
        ASSERT_NE(small_cache.lookup_fast(0x1000), nullptr);
    }
    small_cache.insert(make_block(0x10c0));
    EXPECT_NE(small_cache.lookup_fast(0x1000), nullptr)
        << "fast-path hits must protect a hot translation from cold churn";
    EXPECT_EQ(small_cache.lookup_fast(0x1040), nullptr);
    EXPECT_EQ(small_cache.size(), 3u);
}

TEST_F(BlockCacheTest, SecondChanceRemainsBoundedWhenEveryBlockWasHit) {
    BlockCache small_cache{3};
    for (GuestAddr addr : {0x1000u, 0x1040u, 0x1080u}) {
        small_cache.insert(make_block(addr));
        ASSERT_NE(small_cache.lookup_fast(addr), nullptr);
    }
    small_cache.insert(make_block(0x10c0));
    EXPECT_EQ(small_cache.size(), 3u);
    EXPECT_EQ(small_cache.evictions(), 1u);
    EXPECT_NE(small_cache.lookup_fast(0x10c0), nullptr);
}

TEST_F(BlockCacheTest, FastHitsSurviveRepeatedColdCacheChurn) {
    BlockCache small_cache{3};
    small_cache.insert(make_block(0x1000));
    for (GuestAddr addr = 0x2000; addr < 0x3000; addr += 0x40) {
        ASSERT_NE(small_cache.lookup_fast(0x1000), nullptr);
        small_cache.insert(make_block(addr));
        EXPECT_LE(small_cache.size(), 3u);
    }
    EXPECT_NE(small_cache.lookup_fast(0x1000), nullptr);
}

TEST_F(BlockCacheTest, InvalidateRemovesBlock) {
    cache.insert(make_block(0x1000));
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_TRUE(cache.invalidate(0x1000));
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.lookup(0x1000), nullptr);
}

// 7. InvalidateUnknownReturnsFalse
TEST_F(BlockCacheTest, InvalidateUnknownReturnsFalse) {
    EXPECT_FALSE(cache.invalidate(0x9999));
}

// 8. InvalidateRangeRemovesOverlapping
TEST_F(BlockCacheTest, InvalidateRangeRemovesOverlapping) {
    // Block at 0x1000, length 10 => [0x1000, 0x100A)
    cache.insert(make_block(0x1000, 10));
    // Block at 0x1008, length 10 => [0x1008, 0x1012) — overlaps
    cache.insert(make_block(0x1008, 10));
    // Block at 0x2000, length 10 => [0x2000, 0x200A) — no overlap
    cache.insert(make_block(0x2000, 10));

    // Invalidate range [0x1005, 0x100F) — overlaps first two blocks
    usize removed = cache.invalidate_range(0x1005, 10);
    EXPECT_EQ(removed, 2u);
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_NE(cache.lookup(0x2000), nullptr);
}

// 9. InvalidateRangeIgnoresNonOverlapping
TEST_F(BlockCacheTest, InvalidateRangeIgnoresNonOverlapping) {
    cache.insert(make_block(0x1000, 10));
    cache.insert(make_block(0x2000, 10));

    // Range that doesn't overlap any block
    usize removed = cache.invalidate_range(0x3000, 100);
    EXPECT_EQ(removed, 0u);
    EXPECT_EQ(cache.size(), 2u);
}

// Only a write to executable memory can invalidate translated code: a block
// can be cached for an address only if the guest was allowed to execute it,
// which is PAGE_EXECUTE_READWRITE here. The RX -> RW -> RX patch sequence is
// covered by VirtualProtect notifying on the transition back to executable.
TEST_F(BlockCacheTest, WritableCodeWriteInvalidatesOverlappingBlock) {
    ASSERT_TRUE(memory.map_region(0x4000, 0x1000,
                                  MemoryProtection::ReadWriteExecute));
    JitCompiler compiler(memory);
    compiler.set_block_cache(&cache);
    cache.insert(make_block(0x4100, 10));

    ASSERT_TRUE(memory.write_u8(0x4105, 0x90));
    EXPECT_EQ(cache.lookup(0x4100), nullptr);
}

TEST_F(BlockCacheTest, DataWriteDoesNotInvalidateBlock) {
    ASSERT_TRUE(memory.map_region(0x4000, 0x2000,
                                  MemoryProtection::ReadWriteExecute));
    JitCompiler compiler(memory);
    compiler.set_block_cache(&cache);
    cache.insert(make_block(0x4100, 10));

    ASSERT_TRUE(memory.write_u8(0x5105, 0x90));
    EXPECT_NE(cache.lookup(0x4100), nullptr);
}

TEST_F(BlockCacheTest, RewrittenDecodeRemainsEligibleForFirstCompilation) {
    ASSERT_TRUE(memory.map_region(0x4000, 0x1000,
                                  MemoryProtection::ReadWriteExecute));
    const u8 code[] = {0xB8, 0x2A, 0x00, 0x00, 0x00}; // mov eax,42
    ASSERT_TRUE(memory.write_bytes(0x4100, code, sizeof(code)));
    JitCompiler compiler(memory);
    compiler.set_block_cache(&cache);
    cache.insert(make_block(0x4100, sizeof(code), 1));
    ASSERT_TRUE(memory.write_u8(0x4101, 43));
    ASSERT_EQ(cache.lookup(0x4100), nullptr);
    BasicBlock block = make_block(0x4100, sizeof(code), 1);
    GuestMappedByteReader reader(memory, 0x4100, sizeof(code));
    DecodedInstruction instr;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode32, instr),
              DecodeError::Success);
    block.instructions.push_back(instr);
    EXPECT_NE(compiler.compile(block), nullptr)
        << "rewriting a decoded-only entry must not permanently disable JIT";
}

TEST_F(BlockCacheTest, HighCodePageDoesNotAliasLow32SmcPage) {
    constexpr GuestAddr kLowAlias = 0x00000000;
    constexpr GuestAddr kHighCode = 0x100000000;

    memory.retain_jit_code_page(kHighCode);
    EXPECT_EQ(memory.jit_code_span64_low(), kHighCode);
    EXPECT_EQ(memory.jit_code_span64_high(), kHighCode + 0x1000);
    EXPECT_TRUE(memory.jit_code_pages_overlap(kHighCode + 0x80, 8));
    EXPECT_TRUE(memory.jit_code_pages_overlap(kHighCode - 4, 8))
        << "a store straddling 4 GiB missed the high code page";
    EXPECT_FALSE(memory.jit_code_pages_overlap(kLowAlias + 0x80, 8))
        << "high x64 code page aliased the low-32 page bitmap";
    EXPECT_EQ(memory.jit_code_chunk_refs(kHighCode), 0u)
        << "the compact chunk map owns only low-4-GiB pages";

    memory.release_jit_code_page(kHighCode);
    EXPECT_FALSE(memory.jit_code_pages_overlap(kHighCode, 1));
}

TEST_F(BlockCacheTest, HighCodePageUsesExactPerCacheOwnership) {
    constexpr GuestAddr kLowAlias = 0x00003000;
    constexpr GuestAddr kHighCode = 0x100003100;
    cache.bind_guest_memory(&memory);
    cache.insert(make_block(kHighCode, 16));

    EXPECT_FALSE(cache.range_is_code_free(kHighCode, 1));
    EXPECT_TRUE(cache.range_is_code_free(kLowAlias, 1))
        << "per-cache high code ownership aliased a low guest page";
    EXPECT_TRUE(memory.jit_code_pages_overlap(kHighCode, 1));
    EXPECT_FALSE(memory.jit_code_pages_overlap(kLowAlias, 1));

    EXPECT_TRUE(cache.invalidate(kHighCode));
    EXPECT_FALSE(memory.jit_code_pages_overlap(kHighCode, 1));
}

// 10. ClearEmptiesCache
TEST_F(BlockCacheTest, ClearEmptiesCache) {
    cache.insert(make_block(0x1000));
    cache.insert(make_block(0x2000));
    cache.insert(make_block(0x3000));
    EXPECT_EQ(cache.size(), 3u);

    cache.clear();
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.lookup(0x1000), nullptr);
}

// 11. HitMissCountersCorrect
TEST_F(BlockCacheTest, HitMissCountersCorrect) {
    EXPECT_EQ(cache.total_hits(), 0u);
    EXPECT_EQ(cache.total_misses(), 0u);

    cache.lookup(0x1000);  // miss
    EXPECT_EQ(cache.total_misses(), 1u);

    cache.insert(make_block(0x1000));
    cache.lookup(0x1000);  // hit
    EXPECT_EQ(cache.total_hits(), 1u);

    cache.lookup(0x1000);  // hit
    EXPECT_EQ(cache.total_hits(), 2u);

    cache.lookup(0x9999);  // miss
    EXPECT_EQ(cache.total_misses(), 2u);
}

TEST_F(BlockCacheTest, L1AndHashHitsAreSplit) {
    cache.insert(make_block(0x1000));
    EXPECT_NE(cache.lookup(0x1000), nullptr);
    EXPECT_EQ(cache.l1_hits(), 1u);
    EXPECT_EQ(cache.hash_hits(), 0u);

    // Same L1 slot as 0x1000, so inserting it displaces 0x1000 and the next
    // lookup must fall through to the hash map.
    // The L1 is two-way: one alias shares the set without evicting; the
    // second alias overflows it and displaces 0x1000 (way 1, the oldest).
    const GuestAddr alias = l1_alias_of(0x1000, 0x1000);
    ASSERT_NE(alias, 0u);
    cache.insert(make_block(alias));
    EXPECT_NE(cache.lookup(0x1000), nullptr);
    EXPECT_EQ(cache.hash_hits(), 0u) << "two blocks fit one two-way set";
    const GuestAddr alias2 = l1_alias_of(alias, 0x1000);
    ASSERT_NE(alias2, 0u);
    cache.insert(make_block(alias2));
    EXPECT_NE(cache.lookup(0x1000), nullptr);
    EXPECT_EQ(cache.hash_hits(), 1u);
}

TEST_F(BlockCacheTest, LookupFastFillsL1WithoutAlwaysPromoting) {
    // Force the hash path: steal the sticky slot for 0x1000, then displace
    // it from L1 with a 256 KiB alias. lookup_fast must still find it via
    // the hash map, refill L1, and avoid requiring a splice every time.
    const GuestAddr target = 0x1000;
    GuestAddr sticky_thief = 0;
    for (GuestAddr c = 0x2000; c < 0x800000; c += 0x10) {
        if (sticky_index_of(c) == sticky_index_of(target) &&
            BlockCache::l1_index(c) != BlockCache::l1_index(target)) {
            sticky_thief = c;
            break;
        }
    }
    ASSERT_NE(sticky_thief, 0u);

        const GuestAddr l1_alias = l1_alias_of(target, GuestAddr{1} << 18);
    ASSERT_NE(l1_alias, 0u);
    ASSERT_EQ(BlockCache::l1_index(l1_alias), BlockCache::l1_index(target));
    // Two aliases overflow the two-way set and evict target from L1.
    const GuestAddr l1_alias2 = l1_alias_of(l1_alias, GuestAddr{1} << 18);
    ASSERT_NE(l1_alias2, 0u);
    cache.insert(make_block(target));
    cache.insert(make_block(sticky_thief));  // overwrites sticky for target
    cache.insert(make_block(l1_alias));      // shares the set with target
    cache.insert(make_block(l1_alias2));     // last-hit + L1 = aliases

    const u64 hash_before = cache.hash_hits();
    EXPECT_NE(cache.lookup_fast(target), nullptr);
    EXPECT_EQ(cache.hash_hits(), hash_before + 1u);

    // Next probe is last-hit (no further hash).
    const u64 hash_after = cache.hash_hits();
    EXPECT_NE(cache.lookup_fast(target), nullptr);
    EXPECT_EQ(cache.hash_hits(), hash_after);

    EXPECT_EQ(cache.lookup_fast(0x9999), nullptr);
    EXPECT_EQ(cache.total_misses(), 1u);
}

TEST_F(BlockCacheTest, LookupFastLastHitSkipsL1AndHash) {
    cache.insert(make_block(0x1000));
    EXPECT_NE(cache.lookup_fast(0x1000), nullptr);
    const u64 l1_before = cache.l1_hits();
    const u64 hash_before = cache.hash_hits();
    const u64 hits_before = cache.total_hits();

    // Same RIP again: last-hit compare only — no L1/hash counter bumps.
    EXPECT_NE(cache.lookup_fast(0x1000), nullptr);
    EXPECT_EQ(cache.l1_hits(), l1_before);
    EXPECT_EQ(cache.hash_hits(), hash_before);
    EXPECT_EQ(cache.total_hits(), hits_before + 1u);
}

TEST_F(BlockCacheTest, InstallL1PrefersCoveringBlock) {
    constexpr GuestAddr kCover = 0x1000;
    // Far enough out to need a real alias search, close enough that one
    // decoded block can still span the gap (byte_length below covers it).
    const GuestAddr kSuffix = l1_alias_of(kCover, BlockCache::l1_capacity());
    ASSERT_NE(kSuffix, 0u);
    // Exact-index aliases can still overlap when a decoded block spans a full
    // L1 window. A suffix insert/lookup must not
    // displace the covering block from L1 (JIT linked continues key on
    // start_addr match).
    ASSERT_EQ(BlockCache::l1_index(kCover), BlockCache::l1_index(kSuffix));

    BasicBlock cover = make_block(kCover);
    cover.byte_length = static_cast<u32>(kSuffix - kCover + 16);
    BasicBlock suffix = make_block(kSuffix);
    suffix.byte_length = 8;

    cache.insert(std::move(cover));
    EXPECT_EQ(cache.l1_base()[BlockCache::l1_index(kCover)].block->start_addr,
              kCover);

    cache.insert(std::move(suffix));
    EXPECT_EQ(cache.l1_base()[BlockCache::l1_index(kCover)].block->start_addr,
              kCover);

    EXPECT_NE(cache.lookup(kSuffix), nullptr);
    EXPECT_EQ(cache.l1_base()[BlockCache::l1_index(kCover)].block->start_addr,
              kCover);

    EXPECT_NE(cache.lookup_fast(kSuffix), nullptr);
    EXPECT_EQ(cache.l1_base()[BlockCache::l1_index(kCover)].block->start_addr,
              kCover);
}

TEST_F(BlockCacheTest, LookupFastStickySurvivesL1AliasThrash) {
    // Two starts that share an L1 slot. Warm both, then thrash L1 so each
    // probe misses the direct map; sticky (different mix) should still hit
    // without a hash lookup / LRU splice.
    constexpr GuestAddr kFirst = 0x1000;
    const GuestAddr kAlias = l1_alias_of(kFirst, 0x40000);
    ASSERT_NE(kAlias, 0u);
    ASSERT_EQ(BlockCache::l1_index(kAlias), BlockCache::l1_index(kFirst));
    ASSERT_NE(sticky_index_of(kAlias), sticky_index_of(kFirst));

    cache.insert(make_block(kFirst));
    cache.insert(make_block(kAlias));

    EXPECT_NE(cache.lookup_fast(kFirst), nullptr);
    EXPECT_NE(cache.lookup_fast(kAlias), nullptr);

    const u64 hash_before = cache.hash_hits();
    const u64 hits_before = cache.total_hits();

    // kFirst was displaced from L1 by kAlias; last-hit is kAlias.
    // Sticky must resolve kFirst without another hash hit.
    EXPECT_NE(cache.lookup_fast(kFirst), nullptr);
    EXPECT_EQ(cache.hash_hits(), hash_before);
    EXPECT_EQ(cache.total_hits(), hits_before + 1u);

    // And the other way: last-hit is now kFirst, L1 holds kFirst.
    EXPECT_NE(cache.lookup_fast(kAlias), nullptr);
    EXPECT_EQ(cache.hash_hits(), hash_before);
}

TEST_F(BlockCacheTest, LookupFastStickyRetainsAliasingWorkingSet) {
    // These 32 starts all alias one L1 slot, while the sticky high-bit mix
    // places them in distinct slots. A renderer-sized dispatch working set
    // must stay on the pointer-only path instead of repeatedly traversing the
    // unordered_map after each L1 displacement.
    constexpr GuestAddr kBase = 0x1000;
    constexpr usize kWorkingSet = 32;

    std::vector<GuestAddr> addrs{kBase};
    while (addrs.size() < kWorkingSet) {
        const GuestAddr next = l1_alias_of(addrs.back(), 1);
        ASSERT_NE(next, 0u);
        ASSERT_EQ(BlockCache::l1_index(next), BlockCache::l1_index(kBase));
        addrs.push_back(next);
    }
    for (GuestAddr addr : addrs) cache.insert(make_block(addr));

    const u64 hash_before = cache.hash_hits();
    for (GuestAddr addr : addrs) {
        EXPECT_NE(cache.lookup_fast(addr), nullptr);
    }
    EXPECT_EQ(cache.hash_hits(), hash_before);
}

TEST_F(BlockCacheTest, InvalidateClearsLastHitAndSticky) {
    cache.insert(make_block(0x1000));
    EXPECT_NE(cache.lookup_fast(0x1000), nullptr);
    ASSERT_TRUE(cache.invalidate(0x1000));
    EXPECT_EQ(cache.lookup_fast(0x1000), nullptr);
    EXPECT_EQ(cache.total_misses(), 1u);
}

TEST_F(BlockCacheTest, FindCoveringMidBodyNotExactStart) {
    cache.insert(make_block(0x1000, 10));
    BasicBlock* mid = cache.find_covering(0x1004);
    ASSERT_NE(mid, nullptr);
    EXPECT_EQ(mid->start_addr, 0x1000u);
    // Exact start is not "covering" — diagnostics probe for mid-block only.
    EXPECT_EQ(cache.find_covering(0x1000), nullptr);
    EXPECT_EQ(cache.find_covering(0x100A), nullptr);
    EXPECT_EQ(cache.find_covering(0x2000), nullptr);
}

// 12. BlockWithBranchFlag
TEST_F(BlockCacheTest, BlockWithBranchFlag) {
    BasicBlock b = make_block(0x4000);
    b.ends_with_branch = false;
    cache.insert(std::move(b));

    const BasicBlock* found = cache.lookup(0x4000);
    ASSERT_NE(found, nullptr);
    EXPECT_FALSE(found->ends_with_branch);
}

// 13. InsertDuplicateUpdatesBlock
TEST_F(BlockCacheTest, InsertDuplicateUpdatesBlock) {
    cache.insert(make_block(0x1000, 10, 3));
    EXPECT_EQ(cache.size(), 1u);

    // Insert again with different data at same address
    cache.insert(make_block(0x1000, 20, 7));
    EXPECT_EQ(cache.size(), 1u);

    const BasicBlock* b = cache.lookup(0x1000);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->byte_length, 20u);
    EXPECT_EQ(b->instruction_count, 7u);
}

// An arena retire drops compiled code but keeps the decode. A block that was
// published fully-native no longer HAS a decode (publish_compiled releases it
// once generated code stops referring to it) while instruction_count stays
// set, so keeping that entry would leave it recompilable — and both
// JitCompiler::compile and the interpreter block path walk
// [0, instruction_count) through `instructions`. Such an entry must be evicted
// so the next execution re-decodes.
TEST_F(BlockCacheTest, DropCompiledEvictsEntriesThatLostTheirDecode) {
    u8 fake_code = 0;

    BasicBlock decoded = make_block(0x1000, 10, 2);
    decoded.instructions.resize(2);
    decoded.compiled_code = &fake_code;
    decoded.compiled_link_code = &fake_code;
    decoded.compiled_call_code = &fake_code;
    decoded.compiled_arena_generation = 1;
    cache.insert(std::move(decoded));

    // Fully-native block: code published, decoded IR released.
    BasicBlock released = make_block(0x2000, 10, 2);
    released.instructions.reset();
    released.compiled_code = &fake_code;
    released.compiled_link_code = &fake_code;
    released.compiled_call_code = &fake_code;
    released.compiled_arena_generation = 1;
    cache.insert(std::move(released));

    EXPECT_EQ(cache.drop_compiled_older_than(2), 2u);

    const BasicBlock* kept = cache.lookup(0x1000);
    ASSERT_NE(kept, nullptr);
    EXPECT_EQ(kept->compiled_code, nullptr);
    EXPECT_EQ(kept->compiled_link_code, nullptr);
    EXPECT_EQ(kept->compiled_call_code, nullptr);
    EXPECT_FALSE(kept->compile_failed);
    EXPECT_EQ(kept->instructions.size(), kept->instruction_count);

    EXPECT_EQ(cache.lookup(0x2000), nullptr);
}

TEST(GuestMappedByteReader, StreamsNopSledAcrossPageBoundary) {
    // Intel SDM Vol. 2: instruction fetch is a byte stream; max insn is 15
    // bytes. A 64-NOP sled spanning a guest page must decode from one reused
    // mapped window, not a fresh host_ptr per instruction.
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x4000, 0x2000,
                                  MemoryProtection::ReadWriteExecute));
    std::vector<u8> nops(64, 0x90);
    const GuestAddr start = 0x4000 + 0x1000 - 32;
    ASSERT_TRUE(memory.write_bytes(start, nops.data(), nops.size()).is_ok());
    GuestMappedByteReader reader(memory, start);
    u32 count = 0;
    for (; count < 64; ++count) {
        DecodedInstruction instr;
        ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode32, instr),
                  DecodeError::Success);
        EXPECT_EQ(instr.opcode, Opcode::NOP);
        EXPECT_EQ(instr.length, 1u);
    }
    EXPECT_EQ(reader.position(), 64u);
}

} // namespace

TEST(BlockCacheDecodedArray, CopyOnWriteStillProtectsAnotherHoldersArray) {
    // A handle that is still shared is never modified in place: the mutator
    // detaches onto a private copy first.
    DecodedInsns first;
    DecodedInstruction insn{};
    insn.opcode = Opcode::NOP;
    first.push_back(insn);

    DecodedInsns second = first;               // shares the array
    ASSERT_EQ(first.share(), second.share());

    DecodedInstruction other{};
    other.opcode = Opcode::UD2;
    second.push_back(other);

    EXPECT_NE(first.share(), second.share());
    EXPECT_EQ(first.size(), 1u);
    EXPECT_EQ(second.size(), 2u);
    EXPECT_EQ(first[0].opcode, Opcode::NOP);
}

TEST(BlockCacheHotness, EvictedExecutionReachesSharedThreshold) {
    BlockCache cache{1};
    cache.insert(make_block(0x1000))->execution_count = 1;
    cache.insert(make_block(0x2000));
    auto* restored = cache.insert(make_block(0x1000));
    EXPECT_EQ(restored->execution_count, 1u);
    JitCompiler::set_jit_hot_threshold_for_test(2);
    EXPECT_TRUE(JitCompiler::execution_reaches_hot_threshold(*restored));
    JitCompiler::set_jit_hot_threshold_for_test(0);
}

TEST(BlockCacheHotness, InvalidatedEvictedSpanStartsCold) {
    for (int method = 0; method < 4; ++method) {
        BlockCache cache{1};
        cache.insert(make_block(0x1ffc, 10))->execution_count = 3;
        cache.insert(make_block(0x5000));
        if (method == 0) cache.invalidate(0x1ffc);
        if (method == 1) cache.invalidate_range(0x2000, 1);
        if (method == 2) cache.invalidate_if_cached_page(0x2000, 1);
        if (method == 3) cache.clear();
        EXPECT_EQ(cache.insert(make_block(0x1ffc, 10))->execution_count, 0u);
    }
}

TEST(BlockCacheHotness, BitnessAndSpanChangesStartCold) {
    for (bool change_mode : {false, true}) {
        BlockCache cache{1};
        cache.insert(make_block(0x1000))->execution_count = 3;
        cache.insert(make_block(0x5000));
        auto changed = make_block(0x1000);
        if (change_mode) changed.is_64bit = true;
        else ++changed.byte_length;
        EXPECT_EQ(cache.insert(std::move(changed))->execution_count, 0u);
    }
}

TEST(BlockCacheHotness, HistoryIsBoundedAndUnaffectedByUnrelatedWrites) {
    BlockCache cache{1};
    for (u64 n = 1; n <= 6; ++n) {
        cache.insert(make_block(n * 0x1000))->execution_count = n;
    }
    cache.invalidate_if_cached_page(0x9000, 1);
    EXPECT_EQ(cache.insert(make_block(0x2000))->execution_count, 2u);
    EXPECT_EQ(cache.insert(make_block(0x1000))->execution_count, 0u);
}

TEST(BlockCacheHotness, EvictedHistoryRemainsWriteTrackedUntilInvalidated) {
    BlockCache cache{1};
    cache.insert(make_block(0x1000))->execution_count = 1;
    cache.insert(make_block(0x5000));
    EXPECT_FALSE(cache.range_is_code_free(0x1000, 1));
    EXPECT_EQ(cache.lookup(0x1000), nullptr);
    // Invalidating history must not report a removed live translation.
    EXPECT_EQ(cache.invalidate_if_cached_page(0x1000, 1), 0u);
    EXPECT_TRUE(cache.range_is_code_free(0x1000, 1));
    EXPECT_EQ(cache.insert(make_block(0x1000))->execution_count, 0u);
}

TEST(BlockCacheHotness, CompiledAndFailedBlocksDoNotRetainHistory) {
    for (bool failed : {false, true}) {
        BlockCache cache{1};
        auto block = make_block(0x1000);
        block.execution_count = 5;
        block.compile_failed = failed;
        u32 code = 0;
        if (!failed) block.compiled_code = &code;
        cache.insert(std::move(block));
        cache.insert(make_block(0x5000));
        EXPECT_TRUE(cache.range_is_code_free(0x1000, 1));
        EXPECT_EQ(cache.insert(make_block(0x1000))->execution_count, 0u);
    }
}
