// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Per-block host-memory attribution for the JIT's bookkeeping.
//
// A Steam renderer holds ~500 MB of MALLOC_SMALL in ~4M nodes for ~250k live
// blocks -- ~16 allocations and ~1.9 KB of heap per block -- plus ~1.2 KB of
// arena code per block. This harness reproduces the dispatcher's miss path on
// a synthetic guest program (decode -> BlockCache::insert -> compile ->
// publish -> release decoded) through the real JitCompiler / BlockCache /
// JitCompileMemo / decode memo, and reports host-heap bytes and allocation
// counts per block, split by PHASE:
//
//   decode     per-thread BlockCache entry + decoded array + decode-memo entry
//   compile    memo record + secondary indices + RIP-site table + links +
//              direct patch sites, minus the decoded array released after
//              publish (fully native blocks)
//   adopt      what a SECOND thread pays to reuse the same N blocks through
//              the shared table (its own BlockCache entry, link tables)
//   exit       what the first thread's BlockCache/JitCompiler gave back on
//              destruction, i.e. what was per-thread versus process-wide
//
// Numbers are printed, not asserted, except for the structural invariants at
// the bottom whose expected values follow from the data-structure design.
//
// Knobs (measurement only): VITREO_JITMEM_BLOCKS (default 20000),
// VITREO_JITMEM_DELEGATE_PCT (default 10: share of blocks carrying an
// interpreter-delegate instruction, whose decoded array must be retained),
// VITREO_JITMEM_HOLD=1 (sleep at the end so `heap`/`malloc_history` can
// attribute the live allocations by stack; run with
// MallocStackLogging=malloc).

#include <gtest/gtest.h>

#include <malloc/malloc.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <memory>
#include <thread>
#include <vector>

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_code_arena.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/memory/guest_memory.hpp"
#include "runtime/execution/block_decoder.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

constexpr GuestAddr kCodeBase = 0x00400000;
constexpr usize kStride = 32;

usize env_usize(const char* name, usize fallback) {
    const char* e = std::getenv(name);
    if (e == nullptr || *e == '\0') return fallback;
    const long long v = std::atoll(e);
    return v <= 0 ? fallback : static_cast<usize>(v);
}

/// One synthetic block: eight guest instructions with a mid-block forward
/// Jcc (a trace absorbs it, so the taken leg is a MidJcc direct-link edge)
/// and a JMP to the next block (a JmpRel edge): two linked edges per block.
/// The same bytes decode identically in 32- and 64-bit mode.
///
///   B8 imm32        mov eax, imm          5
///   01 D8           add eax, ebx          2
///   31 C1           xor ecx, eax          2
///   74 03           jz +3                 2   (mid-block forward edge)
///   8B 56 04        mov edx, [esi+4]      3   (a load: cold memory stub)
///   83 E9 01        sub ecx, 1            3
///   8D 14 8A        lea edx, [edx+ecx*4]  3
///   89 46 08        mov [esi+8], eax      3   (a store: cold memory stub)
///                   | 0F A2 90 cpuid; nop     (delegate variant)
///   E9 04 00 00 00  jmp +4 (next block)   5
///   90 90 90 90     padding to the stride 4
void write_block(GuestMemory& mem, GuestAddr addr, u32 imm, bool delegate) {
    u8 code[kStride] = {
        0xB8, static_cast<u8>(imm), static_cast<u8>(imm >> 8),
              static_cast<u8>(imm >> 16), static_cast<u8>(imm >> 24),
        0x01, 0xD8,
        0x31, 0xC1,
        0x74, 0x03,
        0x8B, 0x56, 0x04,
        0x83, 0xE9, 0x01,
        0x8D, 0x14, 0x8A,
        0x89, 0x46, 0x08,
        0xE9, 0x04, 0x00, 0x00, 0x00,
        0x90, 0x90, 0x90, 0x90,
    };
    if (delegate) {
        code[20] = 0x0F;
        code[21] = 0xA2;
        code[22] = 0x90;
    }
    ASSERT_TRUE(mem.write_bytes(addr, code, sizeof(code)));
}

struct HeapSample {
    usize bytes{0};
    usize blocks{0};
    usize arena{0};
};

HeapSample sample_heap() {
    malloc_statistics_t st{};
    malloc_zone_statistics(nullptr, &st);
    return HeapSample{st.size_in_use, st.blocks_in_use,
                      JitCodeArena::arena_bytes_used()};
}

struct PhaseRow {
    const char* name;
    long long bytes;
    long long blocks;
    long long arena;
};

void print_rows(const char* title, usize n, const std::vector<PhaseRow>& rows) {
    std::printf("\n[jitmem] %s  (N = %zu blocks)\n", title, n);
    std::printf("[jitmem] %-10s %12s %10s %12s %10s %10s\n", "phase",
                "heap bytes", "allocs", "arena bytes", "B/block", "alloc/blk");
    for (const PhaseRow& r : rows) {
        std::printf("[jitmem] %-10s %12lld %10lld %12lld %10.1f %10.2f\n",
                    r.name, r.bytes, r.blocks, r.arena,
                    static_cast<double>(r.bytes) / static_cast<double>(n),
                    static_cast<double>(r.blocks) / static_cast<double>(n));
    }
}

struct MemoGuard {
    MemoGuard() {
        JitCompileMemo::clear_compile_memo();
        JitCompileMemo::reset_compile_memo_stats();
        JitCompileMemo::enable_compile_memo();
        JitCompiler::set_shared_adoption_for_test(1);
        // The runtime enables inline block linking once after JIT init; the
        // link tables (direct-edge metadata, patch sites) only exist with it.
        // Every address here is unique to this test's own GuestMemory, and
        // the flag is restored below.
        JitCompiler::enable_jit_link();
        cli::reset_decode_memo();
        reset_decoded_release_stats();
    }
    ~MemoGuard() {
        JitCompiler::disable_jit_link();
        JitCompiler::set_shared_adoption_for_test(-1);
        JitCompileMemo::disable_compile_memo();
        JitCompileMemo::clear_compile_memo();
        cli::reset_decode_memo();
    }
};

/// A dispatcher-shaped guest thread: its own BlockCache and JitCompiler.
struct GuestThread {
    explicit GuestThread(GuestMemory& mem, usize capacity)
        : cache(std::make_unique<BlockCache>(capacity)),
          jit(std::make_unique<JitCompiler>(mem)) {
        jit->set_block_cache(cache.get());
        cache->bind_guest_memory(&mem);
    }
    std::unique_ptr<BlockCache> cache;
    std::unique_ptr<JitCompiler> jit;
};

/// compile_and_publish_dispatch_block() from guest_run_loop.cpp.
bool compile_publish_release(GuestThread& t, BasicBlock& block) {
    CompiledBlock* compiled = t.jit->compile(block);
    if (compiled == nullptr) return false;
    const bool baked = compiled->decoded_owner != nullptr ||
                       compiled->delegate_insns != nullptr;
    t.jit->publish_compiled_block(block, *compiled);
    if (block.compiled_code != nullptr) {
        (void)release_block_decoded(block, baked);
    }
    return block.compiled_code != nullptr;
}

struct Outcome {
    usize fresh{0};
    usize adopted{0};
    usize failed{0};
};

void run_mode(bool is_64bit) {
    const usize n = env_usize("VITREO_JITMEM_BLOCKS", 20000);
    const usize delegate_pct = env_usize("VITREO_JITMEM_DELEGATE_PCT", 10);
    const bool hold = std::getenv("VITREO_JITMEM_HOLD") != nullptr;
    ASSERT_LE(n, usize{65536}) << "must fit one default-capacity cache";

    MemoGuard memo;
    auto memory = std::make_unique<GuestMemory>();
    const usize region = ((n * kStride) + 0xFFFF) & ~usize{0xFFFF};
    ASSERT_TRUE(memory->map_region(kCodeBase, region + 0x10000,
                                   MemoryProtection::ReadWriteExecute));
    for (usize i = 0; i < n; ++i) {
        const bool delegate = (i % 100) < delegate_pct;
        write_block(*memory, kCodeBase + i * kStride,
                    0x1000u + static_cast<u32>(i) * 0x9E3779B9u, delegate);
    }

    std::vector<PhaseRow> rows;
    const HeapSample base = sample_heap();

    // ---- Thread A: decode + insert every block (the cache miss) ----
    auto a = std::make_unique<GuestThread>(*memory, 65536);
    std::vector<BasicBlock*> entries;
    entries.reserve(n);
    for (usize i = 0; i < n; ++i) {
        const GuestAddr addr = kCodeBase + i * kStride;
        BasicBlock decoded = cli::decode_basic_block(*memory, addr, is_64bit);
        ASSERT_GE(decoded.instruction_count, 4u) << "block " << i;
        if (i == 0) std::printf("[jitmem] decoded insns/block: %u\n",
                                decoded.instruction_count);
        entries.push_back(a->cache->insert(std::move(decoded)));
    }
    const HeapSample after_decode = sample_heap();
    rows.push_back({"decode",
                    static_cast<long long>(after_decode.bytes - base.bytes),
                    static_cast<long long>(after_decode.blocks - base.blocks),
                    static_cast<long long>(after_decode.arena - base.arena)});

    // ---- Thread A: compile + publish + release (the dispatcher's compile) --
    Outcome a_out;
    for (BasicBlock* e : entries) {
        if (compile_publish_release(*a, *e)) ++a_out.fresh; else ++a_out.failed;
    }
    const HeapSample after_compile = sample_heap();
    rows.push_back({"compile",
                    static_cast<long long>(after_compile.bytes) -
                        static_cast<long long>(after_decode.bytes),
                    static_cast<long long>(after_compile.blocks) -
                        static_cast<long long>(after_decode.blocks),
                    static_cast<long long>(after_compile.arena) -
                        static_cast<long long>(after_decode.arena)});
    ASSERT_EQ(a_out.failed, 0u);

    // ---- Thread B: reaches the same N addresses (the cold adoption path) ----
    auto b = std::make_unique<GuestThread>(*memory, 65536);
    Outcome b_out;
    for (usize i = 0; i < n; ++i) {
        const GuestAddr addr = kCodeBase + i * kStride;
        if (b->cache->lookup(addr) != nullptr) continue;
        BasicBlock shared;
        if (b->jit->try_adopt_shared_block(addr, is_64bit, shared)) {
            BasicBlock* entry = b->cache->insert(std::move(shared));
            b->jit->link_adopted_block(*entry);
            ++b_out.adopted;
        } else {
            BasicBlock decoded = cli::decode_basic_block(*memory, addr, is_64bit);
            BasicBlock* entry = b->cache->insert(std::move(decoded));
            if (compile_publish_release(*b, *entry)) ++b_out.fresh;
            else ++b_out.failed;
        }
    }
    const HeapSample after_adopt = sample_heap();
    rows.push_back({"adopt",
                    static_cast<long long>(after_adopt.bytes) -
                        static_cast<long long>(after_compile.bytes),
                    static_cast<long long>(after_adopt.blocks) -
                        static_cast<long long>(after_compile.blocks),
                    static_cast<long long>(after_adopt.arena) -
                        static_cast<long long>(after_compile.arena)});

    // ---- Thread A exits ----
    entries.clear();
    a.reset();
    const HeapSample after_exit = sample_heap();
    rows.push_back({"A exit",
                    static_cast<long long>(after_exit.bytes) -
                        static_cast<long long>(after_adopt.bytes),
                    static_cast<long long>(after_exit.blocks) -
                        static_cast<long long>(after_adopt.blocks),
                    static_cast<long long>(after_exit.arena) -
                        static_cast<long long>(after_adopt.arena)});
    rows.push_back({"live",
                    static_cast<long long>(after_exit.bytes - base.bytes),
                    static_cast<long long>(after_exit.blocks - base.blocks),
                    static_cast<long long>(after_exit.arena - base.arena)});

    print_rows(is_64bit ? "x64 guest" : "x86 guest", n, rows);
    const auto cm = JitCompileMemo::compile_memo_stats();
    const auto dm = cli::decode_memo_stats();
    const auto rs = decoded_release_stats();
    std::printf("[jitmem] A fresh=%zu failed=%zu | B adopted=%zu fresh=%zu "
                "failed=%zu\n", a_out.fresh, a_out.failed, b_out.adopted,
                b_out.fresh, b_out.failed);
    std::printf("[jitmem] compile memo: records=%zu fresh_emits=%llu "
                "cold_adoptions=%llu | decode memo: entries=%zu strong=%zu "
                "retained=%zu B | released arrays=%llu (%llu B) "
                "retained_delegate=%llu\n",
                cm.records, static_cast<unsigned long long>(cm.fresh_emits),
                static_cast<unsigned long long>(cm.cold_adoptions),
                dm.entries, dm.strong_entries, dm.retained_bytes,
                static_cast<unsigned long long>(rs.releases),
                static_cast<unsigned long long>(rs.released_bytes),
                static_cast<unsigned long long>(rs.retained_delegate));
    std::printf("[jitmem] sizeof: BasicBlock=%zu DecodedInstruction=%zu "
                "CompiledKey=%zu CompiledRecord=%zu CompiledBlock=%zu\n",
                sizeof(BasicBlock), sizeof(DecodedInstruction),
                sizeof(JitCompileMemo::CompiledKey),
                sizeof(JitCompileMemo::CompiledRecord), sizeof(CompiledBlock));
    std::fflush(stdout);

    // Structural invariants (design facts, not measured constants):
    //  - every block was compiled exactly once process-wide and adopted by
    //    the second thread without a fresh emit;
    //  - exactly the delegate share retains its decoded array through the
    //    cache entry (publish_compiled drops a fully native block's array
    //    itself, so release_block_decoded counts nothing for those).
    const usize delegate_blocks = (n / 100) * delegate_pct +
                                  std::min(n % 100, delegate_pct);
    EXPECT_EQ(cm.fresh_emits, n);
    EXPECT_EQ(b_out.adopted, n);
    EXPECT_EQ(b_out.fresh, 0u);
    EXPECT_EQ(rs.retained_delegate, delegate_blocks);
    EXPECT_EQ(rs.releases, 0u);
    // Allocation counts per block follow from the containers each phase
    // touches, so a ceiling on them catches a reintroduced per-block table
    // (the kind of regression a byte figure hides behind malloc rounding):
    //   decode   LRU node, address map node, page vector growth (amortised
    //            < 1), decoded array + its control block, decode-memo node:
    //            5 + amortised page/bucket growth;
    //   compile  compile-memo node, RIP block, memo link vector, and one
    //            patch target node per linked edge: 6 (two edges here);
    //   adopt    LRU node, address map node, a patch target per edge: 5.
    // Measured 5.02 / 4.12 / 3.02 after the 2026-10-01 reductions, from
    // 6.02 / 8.12 / 6.02 before them, with ONE linked edge per block: the jz
    // while only MidJcc was linked by default, then the jmp -- a mid-block
    // exit got a slot only if the block had none yet, and the terminal's is
    // emitted first. Linking both measured 5.02 / 5.12 / 4.02 (2026-10-08):
    // exactly one more patch target per block per thread.
    const auto per_block = [n](long long blocks) {
        return static_cast<double>(blocks) / static_cast<double>(n);
    };
    EXPECT_LE(per_block(rows[0].blocks), 6.0) << "decode allocations/block";
    EXPECT_LE(per_block(rows[1].blocks), 6.0) << "compile allocations/block";
    EXPECT_LE(per_block(rows[2].blocks), 5.0) << "adopt allocations/block";

    if (hold) {
        std::printf("[jitmem] pid %d holding for 120 s\n", getpid());
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(120));
    }
    b.reset();
}

}  // namespace

TEST(JitMemoryFootprint, PerBlockBytesByPhaseX86) { run_mode(false); }
TEST(JitMemoryFootprint, PerBlockBytesByPhaseX64) { run_mode(true); }
