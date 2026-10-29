// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// A guest thread whose working set exceeds its BlockCache keeps evicting
// blocks and re-entering them: lookup miss, adoption from the process-wide
// compile memo, insert (which evicts), and re-linking. These tests drive that
// cycle exactly as prepare_dispatch_block() does.
//
// The DISABLED_ benchmarks measure it (run with
// --gtest_also_run_disabled_tests --gtest_filter='BlockCacheReentryBench.*').

#include <gtest/gtest.h>

#include <malloc/malloc.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/jit_stats.hpp"
#include "runtime/execution/block_decoder.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using vitreo::cli::decode_basic_block;
using vitreo::cli::decode_memo_stats;
using vitreo::cli::rematerialize_block_instructions;
using vitreo::cli::reset_decode_memo;

namespace {

constexpr GuestAddr kBase = 0x00400000;
constexpr GuestAddr kStride = 32;

struct MemoGuard {
    MemoGuard() {
        JitCompileMemo::clear_compile_memo();
        JitCompileMemo::reset_compile_memo_stats();
        JitCompileMemo::enable_compile_memo();
        JitCompiler::set_shared_adoption_for_test(1);
        JitCompiler::enable_jit_link();
    }
    ~MemoGuard() {
        JitCompiler::disable_jit_link();
        JitCompiler::set_shared_adoption_for_test(-1);
        JitCompileMemo::disable_compile_memo();
        JitCompileMemo::clear_compile_memo();
    }
};

GuestAddr block_addr(usize i) { return kBase + static_cast<GuestAddr>(i) * kStride; }

/// `add eax,1 ; xor ecx,ebx ; add edx,ecx ; jmp block(i+1)` with block n-1
/// jumping back to block 0: one fully native block per stride, each linked
/// to the next.
void write_chain(GuestMemory& memory, usize n) {
    std::vector<u8> bytes(n * kStride, 0xCC);
    for (usize i = 0; i < n; ++i) {
        u8* p = bytes.data() + i * kStride;
        const GuestAddr here = block_addr(i);
        const GuestAddr next = block_addr((i + 1) % n);
        const u8 body[] = {0x83, 0xC0, 0x01, 0x31, 0xD9, 0x01, 0xCA, 0xE9};
        std::copy(std::begin(body), std::end(body), p);
        const i32 rel = static_cast<i32>(static_cast<i64>(next) -
                                         static_cast<i64>(here + 12));
        std::memcpy(p + 8, &rel, sizeof(rel));
    }
    ASSERT_TRUE(memory.write_bytes(kBase, bytes.data(), bytes.size()));
}

/// The dispatcher's miss path (prepare_dispatch_block) minus the runtime
/// probe vetoes, which never fire here.
struct Dispatch {
    GuestMemory& memory;
    BlockCache& cache;
    JitCompiler& jit;
    u64 adopted{0};
    u64 decoded{0};

    BasicBlock* resolve(GuestAddr addr) {
        BasicBlock* block = cache.lookup(addr);
        bool adopted_shared = false;
        if (block == nullptr) {
            BasicBlock shared;
            if (jit.try_adopt_shared_block(addr, false, shared)) {
                block = cache.insert(std::move(shared));
                adopted_shared = true;
                ++adopted;
            } else {
                block = cache.insert(decode_basic_block(memory, addr, false));
                ++decoded;
            }
        }
        if (adopted_shared && block->compiled_code != nullptr) {
            jit.link_adopted_block(*block);
        }
        return block;
    }
};

/// Phase timers for one resolve, in mach ticks.
struct Phases {
    u64 lookup{0}, adopt{0}, insert{0}, link{0}, decode{0};
};

class BlockCacheReentryBench : public ::testing::Test {
protected:
    static usize env_usize(const char* name, usize fallback) {
        const char* v = std::getenv(name);
        if (v == nullptr || *v == '\0') return fallback;
        return static_cast<usize>(std::strtoull(v, nullptr, 0));
    }

    void SetUp() override {
        m_cap = env_usize("VITREO_BENCH_CAP", 65536);
        m_over = m_cap * 4;
        m_memory = std::make_unique<GuestMemory>();
        const usize bytes = (m_over * kStride + 0xFFFF) & ~usize{0xFFFF};
        ASSERT_TRUE(m_memory->map_region(kBase, bytes,
                                         MemoryProtection::ReadWrite));
        write_chain(*m_memory, m_over);
    }

    /// Publish every block into the process-wide memo from a throwaway
    /// thread-state, as other guest threads would have.
    void warm_memo(usize n) {
        BlockCache cache(n + 16);
        JitCompiler jit(*m_memory);
        jit.set_block_cache(&cache);
        for (usize i = 0; i < n; ++i) {
            BasicBlock* entry =
                cache.insert(decode_basic_block(*m_memory, block_addr(i), false));
            CompiledBlock* compiled = jit.compile(*entry);
            ASSERT_NE(compiled, nullptr) << i;
            jit.publish_compiled_block(*entry, *compiled);
        }
    }

    static double ns(u64 ticks) {
        static const mach_timebase_info_data_t tb = [] {
            mach_timebase_info_data_t t{};
            mach_timebase_info(&t);
            return t;
        }();
        return static_cast<double>(ticks) * tb.numer / tb.denom;
    }

    /// ns per resolve over `passes` timed passes of `n` blocks, after one
    /// untimed warm pass.
    double run_loop(usize n, usize passes, Phases* phases = nullptr) {
        BlockCache cache(m_cap);
        JitCompiler jit(*m_memory);
        jit.set_block_cache(&cache);
        Dispatch d{*m_memory, cache, jit};
        for (usize i = 0; i < n; ++i) (void)d.resolve(block_addr(i));
        const u64 t0 = mach_absolute_time();
        for (usize p = 0; p < passes; ++p) {
            for (usize i = 0; i < n; ++i) {
                if (phases == nullptr) {
                    (void)d.resolve(block_addr(i));
                    continue;
                }
                const GuestAddr addr = block_addr(i);
                u64 a = mach_absolute_time();
                BasicBlock* block = cache.lookup(addr);
                u64 b = mach_absolute_time();
                phases->lookup += b - a;
                if (block != nullptr) continue;
                BasicBlock shared;
                a = mach_absolute_time();
                const bool ok = jit.try_adopt_shared_block(addr, false, shared);
                b = mach_absolute_time();
                phases->adopt += b - a;
                if (!ok) {
                    a = mach_absolute_time();
                    block = cache.insert(decode_basic_block(*m_memory, addr, false));
                    phases->decode += mach_absolute_time() - a;
                    continue;
                }
                a = mach_absolute_time();
                block = cache.insert(std::move(shared));
                b = mach_absolute_time();
                phases->insert += b - a;
                jit.link_adopted_block(*block);
                phases->link += mach_absolute_time() - b;
            }
        }
        const u64 t1 = mach_absolute_time();
        return ns(t1 - t0) / static_cast<double>(n * passes);
    }

    /// A resident hot set (half the cap, re-hit three times per step) beside
    /// a cold stream that never fits: every stream resolve evicts, and the
    /// victim search has to step past referenced hot entries. ns per step.
    double run_mixed(usize steps) {
        BlockCache cache(m_cap);
        JitCompiler jit(*m_memory);
        jit.set_block_cache(&cache);
        Dispatch d{*m_memory, cache, jit};
        const usize hot = m_cap / 2;
        const usize stream = m_over - hot;
        usize s = 0, h = 0;
        const auto step = [&] {
            (void)d.resolve(block_addr(hot + s));
            if (++s == stream) s = 0;
            for (int j = 0; j < 3; ++j) {
                (void)d.resolve(block_addr(h));
                if (++h == hot) h = 0;
            }
        };
        for (usize i = 0; i < m_cap; ++i) step();
        const u64 t0 = mach_absolute_time();
        for (usize i = 0; i < steps; ++i) step();
        return ns(mach_absolute_time() - t0) / static_cast<double>(steps);
    }

    usize m_cap{0};
    usize m_over{0};
    std::unique_ptr<GuestMemory> m_memory;
};

// ---------------------------------------------------------------------------
// Eviction then re-entry correctness. Every test runs both arms of
// VITREO_JIT_REENTRY_LEGACY: the two must be observably identical.
// ---------------------------------------------------------------------------

struct LegacyArm {
    explicit LegacyArm(int legacy) { set_reentry_legacy_for_test(legacy); }
    ~LegacyArm() { set_reentry_legacy_for_test(-1); }
};

class BlockCacheReentryTest : public ::testing::TestWithParam<int> {
protected:
    static constexpr usize kBlocks = 8;
    static constexpr u8 EAX = static_cast<u8>(RegIndex::RAX);

    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(kBase, 0x10000,
                                         MemoryProtection::ReadWrite));
        write_chain(*m_memory, kBlocks);
    }

    /// Publish blocks into the process-wide memo from a thread-state that is
    /// gone before the test's own thread-state exists.
    void warm(std::initializer_list<GuestAddr> addrs) {
        BlockCache cache(64);
        JitCompiler jit(*m_memory);
        jit.set_block_cache(&cache);
        for (GuestAddr addr : addrs) {
            BasicBlock* entry =
                cache.insert(decode_basic_block(*m_memory, addr, false));
            CompiledBlock* compiled = jit.compile(*entry);
            ASSERT_NE(compiled, nullptr) << std::hex << addr;
            jit.publish_compiled_block(*entry, *compiled);
        }
    }
    void warm_chain() {
        for (usize i = 0; i < kBlocks; ++i) warm({block_addr(i)});
    }

    std::unique_ptr<GuestMemory> m_memory;
};

INSTANTIATE_TEST_SUITE_P(Arms, BlockCacheReentryTest,
                         ::testing::Values(0, 1),
                         [](const ::testing::TestParamInfo<int>& info) {
                             return info.param ? "Legacy" : "Default";
                         });

// A start re-inserted after eviction is on its pages exactly once, for both a
// plain eviction and one that left a hotness span holding the page: removing
// every block afterwards must return each page's count to zero.
TEST_P(BlockCacheReentryTest, ReinsertAfterEvictionKeepsThePageIndexExact) {
    LegacyArm arm(GetParam());
    BlockCache cache(4);
    cache.bind_guest_memory(m_memory.get());
    for (int round = 0; round < 3; ++round) {
        for (usize i = 0; i < kBlocks; ++i) {
            BasicBlock block = decode_basic_block(*m_memory, block_addr(i), false);
            // Odd blocks have run, so their eviction keeps a hotness span.
            block.execution_count = (i & 1) ? 1 : 0;
            ASSERT_NE(cache.insert(std::move(block)), nullptr);
        }
    }
    ASSERT_EQ(cache.size(), 4u);
    const usize span = kBlocks * kStride;
    EXPECT_FALSE(cache.range_is_code_free(kBase, span));
    // A mid-body address resolves to its one covering resident block.
    BasicBlock* covering = cache.find_covering(block_addr(kBlocks - 1) + 2);
    ASSERT_NE(covering, nullptr);
    EXPECT_EQ(covering->start_addr, block_addr(kBlocks - 1));

    std::vector<GuestAddr> removed;
    EXPECT_EQ(cache.invalidate_range(kBase, span, &removed), 4u);
    std::sort(removed.begin(), removed.end());
    EXPECT_EQ(removed, (std::vector<GuestAddr>{block_addr(4), block_addr(5),
                                               block_addr(6), block_addr(7)}));
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_TRUE(cache.range_is_code_free(kBase, span))
        << "a page still counts a block or span that is gone";
    EXPECT_EQ(cache.code_page_map()[kBase >> 12], 0u);
    EXPECT_FALSE(m_memory->jit_code_pages_overlap(kBase, span));
}

// A guest write after a block was evicted must not be served the old
// translation on re-entry: adoption checks the bytes, and the new decode is
// of the new instruction.
TEST_P(BlockCacheReentryTest, RewriteAfterEvictionReentersTheNewBytes) {
    LegacyArm arm(GetParam());
    MemoGuard memo;
    warm_chain();
    BlockCache cache(4);
    JitCompiler jit(*m_memory);
    jit.set_block_cache(&cache);
    Dispatch d{*m_memory, cache, jit};
    for (usize i = 0; i < kBlocks; ++i) ASSERT_NE(d.resolve(block_addr(i)), nullptr);
    ASSERT_EQ(d.adopted, kBlocks);
    ASSERT_EQ(cache.lookup(block_addr(0)), nullptr) << "block 0 was not evicted";

    // add eax, 1 -> add eax, 2 (same length, same block shape).
    const u8 patched[] = {0x83, 0xC0, 0x02};
    ASSERT_TRUE(m_memory->write_bytes(block_addr(0), patched, sizeof(patched)));
    m_memory->notify_executable_write(block_addr(0), sizeof(patched));
    (void)jit.synchronize_executable_writes();

    BasicBlock* again = d.resolve(block_addr(0));
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(d.decoded, 1u) << "the stale record was adopted";
    EXPECT_EQ(again->compiled_code, nullptr);
    ASSERT_EQ(again->instructions.size(), again->instruction_count);
    ASSERT_GE(again->instruction_count, 1u);
    const DecodedInstruction& add = again->instructions[0];
    ASSERT_GE(add.operand_count, 2u);
    EXPECT_EQ(add.operands[1].imm, 2);
}

// Eviction does not unpatch a direct link into code that is still live, and
// re-adopting both ends re-patches nothing. A later rewrite of the target
// still fails the link closed.
TEST_P(BlockCacheReentryTest, DirectLinkIntoAnEvictedTargetStaysLiveUntilRewritten) {
    LegacyArm arm(GetParam());
    if (!JitCompiler::link_edge_enabled(JitCompiler::LinkEdge::JmpRel)) {
        GTEST_SKIP() << "JMP rel direct links are disabled";
    }
    // block 0: add eax,1; jmp 1   block 1: add eax,1; jmp 2 (never compiled)
    MemoGuard memo;
    warm({block_addr(0), block_addr(1), block_addr(3), block_addr(4),
          block_addr(5)});
    BlockCache cache(2);
    JitCompiler jit(*m_memory);
    jit.set_block_cache(&cache);
    Dispatch d{*m_memory, cache, jit};
    ASSERT_NE(d.resolve(block_addr(1)), nullptr);
    BasicBlock* source = d.resolve(block_addr(0));
    ASSERT_NE(source, nullptr);
    ASSERT_NE(source->compiled_code, nullptr);
    CompiledBlock source_code = compiled_from_cache(*source);

    // Push both out of the cache, which also empties their L1 slots.
    for (GuestAddr filler : {block_addr(3), block_addr(4), block_addr(5)}) {
        ASSERT_NE(d.resolve(filler), nullptr);
    }
    ASSERT_EQ(cache.lookup(block_addr(0)), nullptr);
    ASSERT_EQ(cache.lookup(block_addr(1)), nullptr);

    const auto run_source = [&](u32& eax) {
        CpuState cpu;
        cpu.init_protected_mode();
        cpu.gpr.set32(static_cast<u8>(RegIndex::RSP), 0x8000);
        cpu.gpr.set32(EAX, 0);
        cpu.rip = block_addr(0);
        const JitExecResult r = jit.execute(source_code, cpu);
        eax = cpu.gpr.get32(EAX);
        return std::pair<JitExecResult, GuestAddr>{r, cpu.rip};
    };
    u32 eax = 0;
    auto [r1, rip1] = run_source(eax);
    EXPECT_EQ(r1, JitExecResult::Continue);
    EXPECT_EQ(eax, 2u) << "the patched edge into the evicted target was lost";
    EXPECT_EQ(rip1, block_addr(2));

    const u64 patches = stats::link_patches_performed();
    ASSERT_NE(d.resolve(block_addr(1)), nullptr);
    ASSERT_NE(d.resolve(block_addr(0)), nullptr);
    EXPECT_EQ(stats::link_patches_performed(), patches)
        << "re-adoption re-patched an edge that was already current";

    // Rewrite the target: add eax,1 -> add eax,0x20.
    const u8 patched[] = {0x83, 0xC0, 0x20};
    ASSERT_TRUE(m_memory->write_bytes(block_addr(1), patched, sizeof(patched)));
    m_memory->notify_executable_write(block_addr(1), sizeof(patched));
    (void)jit.synchronize_executable_writes();
    auto [r2, rip2] = run_source(eax);
    EXPECT_EQ(r2, JitExecResult::Continue);
    EXPECT_EQ(eax, 1u) << "the source still entered the rewritten target's old code";
    EXPECT_EQ(rip2, block_addr(1));
}

// An adopted fully native block carries no decoded array. When the
// interpreter needs one after a re-entry, rematerializing reproduces the
// decode exactly, and refuses when the bytes no longer match the entry.
TEST_P(BlockCacheReentryTest, ReleasedArrayIsRebuiltForTheInterpreterAfterReentry) {
    LegacyArm arm(GetParam());
    MemoGuard memo;
    warm_chain();
    BlockCache cache(2);
    JitCompiler jit(*m_memory);
    jit.set_block_cache(&cache);
    Dispatch d{*m_memory, cache, jit};
    for (usize i = 0; i < 3; ++i) ASSERT_NE(d.resolve(block_addr(i)), nullptr);
    ASSERT_EQ(cache.lookup(block_addr(0)), nullptr);
    BasicBlock* entry = d.resolve(block_addr(0));
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->compiled_code, nullptr);
    EXPECT_TRUE(entry->instructions_released);
    EXPECT_TRUE(entry->instructions.empty());
    ASSERT_EQ(entry->instruction_count, 4u);

    const BasicBlock fresh = decode_basic_block(*m_memory, block_addr(0), false);
    ASSERT_TRUE(rematerialize_block_instructions(*m_memory, *entry));
    ASSERT_EQ(entry->instructions.size(), fresh.instructions.size());
    for (usize i = 0; i < fresh.instructions.size(); ++i) {
        EXPECT_EQ(entry->instructions[i].opcode, fresh.instructions[i].opcode) << i;
        EXPECT_EQ(entry->instructions[i].length, fresh.instructions[i].length) << i;
    }

    // Re-enter block 1 (released again), then change its shape underneath:
    // xor ecx,ebx -> jmp +0 ends the block two bytes early.
    ASSERT_NE(d.resolve(block_addr(2)), nullptr);
    ASSERT_NE(d.resolve(block_addr(3)), nullptr);
    BasicBlock* stale = d.resolve(block_addr(1));
    ASSERT_NE(stale, nullptr);
    ASSERT_TRUE(stale->instructions.empty());
    const u8 jmp0[] = {0xEB, 0x00};
    ASSERT_TRUE(m_memory->write_bytes(block_addr(1) + 3, jmp0, sizeof(jmp0)));
    EXPECT_FALSE(rematerialize_block_instructions(*m_memory, *stale))
        << "an array for different bytes was handed to the interpreter";
}

// The decode memo's expiry sweep is amortized: past 2^17 live entries a
// sweep that frees nothing used to repeat on every insert.
TEST(BlockDecodeMemoSweep, InsertsPastTheSweepTriggerStayLinear) {
    LegacyArm arm(0);
    reset_decode_memo();
    constexpr GuestAddr kMemoBase = 0x01000000;
    constexpr GuestAddr kMemoStride = 8;
    constexpr usize kCount = (usize{1} << 17) + 4096;
    auto memory = std::make_unique<GuestMemory>();
    const usize bytes = (kCount * kMemoStride + 0xFFFF) & ~usize{0xFFFF};
    ASSERT_TRUE(memory->map_region(kMemoBase, bytes, MemoryProtection::ReadWrite));
    // One `jmp $` per stride: a one-instruction block each.
    std::vector<u8> code(kCount * kMemoStride, 0xCC);
    for (usize i = 0; i < kCount; ++i) {
        code[i * kMemoStride] = 0xEB;
        code[i * kMemoStride + 1] = 0xFE;
    }
    ASSERT_TRUE(memory->write_bytes(kMemoBase, code.data(), code.size()));
    const auto decode_range = [&](usize from, usize to) {
        const u64 t0 = mach_absolute_time();
        for (usize i = from; i < to; ++i) {
            (void)decode_basic_block(*memory, kMemoBase + i * kMemoStride, false);
        }
        return mach_absolute_time() - t0;
    };
    const u64 first = decode_range(0, 4096);
    (void)decode_range(4096, kCount - 4096);
    const u64 last = decode_range(kCount - 4096, kCount);
    EXPECT_GE(decode_memo_stats().entries, usize{1} << 17);
    EXPECT_LT(last, first * 25 + 1)
        << "decodes past the sweep trigger cost " << last << " ticks vs "
        << first << " for the first 4096";
    reset_decode_memo();
}

TEST_F(BlockCacheReentryBench, DISABLED_PhaseBreakdown) {
    MemoGuard memo;
    warm_memo(m_over);
    Phases ph;
    const usize passes = env_usize("VITREO_BENCH_PASSES", 2);
    const double per = run_loop(m_over, passes, &ph);
    const double calls = static_cast<double>(m_over * passes);
    std::printf("[bcache-bench] over-cap n=%zu cap=%zu: %.1f ns/resolve (timed)\n"
                "  lookup %.1f  adopt %.1f  insert %.1f  link %.1f  decode %.1f ns\n",
                m_over, m_cap, per, ns(ph.lookup) / calls, ns(ph.adopt) / calls,
                ns(ph.insert) / calls, ns(ph.link) / calls, ns(ph.decode) / calls);
}

/// Long untimed loop for `sample`: VITREO_BENCH_MIXED=1 selects the mixed
/// load, VITREO_BENCH_PASSES the length.
TEST_F(BlockCacheReentryBench, DISABLED_SampleLoop) {
    MemoGuard memo;
    warm_memo(m_over);
    const usize passes = env_usize("VITREO_BENCH_PASSES", 50);
    const double per = env_usize("VITREO_BENCH_MIXED", 0) != 0
        ? run_mixed(m_over * passes)
        : run_loop(m_over, passes);
    std::printf("[bcache-bench] sample loop: %.1f ns\n", per);
}

/// The gate: interleaved A/B pairs of the over-cap loop, the loop just above
/// the cap, and an in-cap loop. Arm A is the legacy behaviour
/// (VITREO_JIT_LEGACY_REENTRY semantics), arm B the default.
TEST_F(BlockCacheReentryBench, DISABLED_InterleavedPairs) {
    MemoGuard memo;
    warm_memo(m_over);
    const usize pairs = env_usize("VITREO_BENCH_PAIRS", 5);
    const usize just_above = m_cap + m_cap / 16;
    const usize in_cap = m_cap * 3 / 4;
    struct Load { const char* name; usize n; usize passes; };
    const Load loads[] = {{"over-cap", m_over, 2},
                          {"just-above", just_above, 4},
                          {"in-cap", in_cap, 400},
                          {"mixed", 0, 0}};
    for (const Load& load : loads) {
        double sum[2] = {0, 0};
        for (usize p = 0; p < pairs; ++p) {
            double r[2];
            for (int arm = 0; arm < 2; ++arm) {
                const int a = (p & 1) ? 1 - arm : arm;
                set_reentry_legacy_for_test(a == 0 ? 1 : 0);
                r[a] = load.n == 0 ? run_mixed(m_over * 2)
                                   : run_loop(load.n, load.passes);
            }
            sum[0] += r[0];
            sum[1] += r[1];
            std::printf("[bcache-bench] %-10s pair %zu: legacy %.1f  new %.1f ns/resolve "
                        "(%+.1f%%)\n",
                        load.name, p, r[0], r[1], 100.0 * (r[1] - r[0]) / r[0]);
        }
        std::printf("[bcache-bench] %-10s mean: legacy %.1f  new %.1f (%+.1f%%)\n",
                    load.name, sum[0] / pairs, sum[1] / pairs,
                    100.0 * (sum[1] - sum[0]) / sum[0]);
    }
    set_reentry_legacy_for_test(-1);
}

/// Host heap bytes per cached block, adopted (fully native) and decoded
/// (interpreted, retaining its array), with a full cache.
TEST_F(BlockCacheReentryBench, DISABLED_MemoryPerEntry) {
    MemoGuard memo;
    warm_memo(m_cap);
    const auto in_use = [] {
        malloc_statistics_t st{};
        malloc_zone_statistics(nullptr, &st);
        return st.size_in_use;
    };
    for (int arm = 0; arm < 2; ++arm) {
        set_reentry_legacy_for_test(arm == 0 ? 1 : 0);
        const usize before = in_use();
        {
            auto cache = std::make_unique<BlockCache>(m_cap);
            auto jit = std::make_unique<JitCompiler>(*m_memory);
            jit->set_block_cache(cache.get());
            Dispatch d{*m_memory, *cache, *jit};
            for (usize i = 0; i < m_cap; ++i) (void)d.resolve(block_addr(i));
            const usize full = in_use();
            std::printf("[bcache-bench] %s adopted: %.1f B/entry (cache=%zu)\n",
                        arm == 0 ? "legacy" : "new",
                        static_cast<double>(full - before) / m_cap, cache->size());
        }
        const usize before_dec = in_use();
        {
            auto cache = std::make_unique<BlockCache>(m_cap);
            for (usize i = 0; i < m_cap; ++i) {
                cache->insert(decode_basic_block(*m_memory, block_addr(i), false));
            }
            const usize full = in_use();
            std::printf("[bcache-bench] %s decoded: %.1f B/entry\n",
                        arm == 0 ? "legacy" : "new",
                        static_cast<double>(full - before_dec) / m_cap);
        }
    }
    set_reentry_legacy_for_test(-1);
}

}  // namespace
