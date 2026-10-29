/// @file test_jit_code_page_scan.cpp
/// @brief GuestMemory::jit_code_pages_overlap() — the SMC bookkeeping probe the
///        JIT store helpers run before publishing a region into the inline
///        store fast path.
///
/// Measured on a Chrome browser-process startup profile this single function
/// was 8.02% of CPU-bound time — on its own more than half of what all emitted
/// guest code cost. It was a page-by-page walk of the *whole* published region
/// on *every* store that missed the inline path, with no coarse reject and no
/// memo, so a region that can never be published (it holds compiled code) paid
/// a full walk per store forever.
///
/// These tests pin the two properties the optimisation must not break:
///   - it must never under-report (that is the direction that lets the JIT run
///     a stale translation of rewritten guest bytes), and
///   - the memo must expire whenever the page map changes.
/// Plus a scaling measurement, so a regression that restores the linear walk is
/// visible as a number rather than as a vibe.

#include <gtest/gtest.h>

#include "core/memory/guest_memory.hpp"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace vitreo;

namespace {

constexpr GuestAddr kBase = 0x0400'0000;
// Large enough that a page-by-page walk and a chunk-skipping walk differ by
// three orders of magnitude: 256 MiB is 65536 pages but only 128 chunks.
constexpr usize kBigRegion = 256ull * 1024 * 1024;

class JitCodePageScanTest : public ::testing::Test {
protected:
    void SetUp() override { m_mem = std::make_unique<GuestMemory>(); }
    std::unique_ptr<GuestMemory> m_mem;
};

}  // namespace

// ---------------------------------------------------------------------------
// Correctness: the answer itself, independent of any fast path.
// ---------------------------------------------------------------------------

TEST_F(JitCodePageScanTest, EmptyMapReportsNoOverlap) {
    EXPECT_FALSE(m_mem->jit_code_pages_overlap(kBase, kBigRegion));
}

TEST_F(JitCodePageScanTest, ZeroSizeNeverOverlaps) {
    m_mem->retain_jit_code_page(kBase);
    EXPECT_FALSE(m_mem->jit_code_pages_overlap(kBase, 0));
}

TEST_F(JitCodePageScanTest, RetainedPageIsFoundAtEveryPositionInRange) {
    // Front, middle and back of the range, so a fast path that clips an end
    // (an off-by-one in the envelope) is caught rather than averaged away.
    const GuestAddr front = kBase;
    const GuestAddr middle = kBase + kBigRegion / 2;
    const GuestAddr back = kBase + kBigRegion - 0x1000;
    for (GuestAddr page : {front, middle, back}) {
        m_mem->retain_jit_code_page(page);
        EXPECT_TRUE(m_mem->jit_code_pages_overlap(kBase, kBigRegion))
            << "code page 0x" << std::hex << page
            << " inside the queried range was not reported";
        EXPECT_TRUE(m_mem->jit_code_pages_overlap(page, 1));
        m_mem->release_jit_code_page(page);
        EXPECT_FALSE(m_mem->jit_code_pages_overlap(kBase, kBigRegion));
    }
}

TEST_F(JitCodePageScanTest, PageJustOutsideRangeIsNotReported) {
    m_mem->retain_jit_code_page(kBase - 0x1000);
    EXPECT_FALSE(m_mem->jit_code_pages_overlap(kBase, 0x1000));
    m_mem->retain_jit_code_page(kBase + 0x1000);
    EXPECT_FALSE(m_mem->jit_code_pages_overlap(kBase, 0x1000));
    // ... but the page itself, once marked, is.
    m_mem->retain_jit_code_page(kBase);
    EXPECT_TRUE(m_mem->jit_code_pages_overlap(kBase, 0x1000));
}

TEST_F(JitCodePageScanTest, UnalignedRangeCoversItsLastPartialPage) {
    m_mem->retain_jit_code_page(kBase + 0x2000);
    // Range ends one byte into the marked page.
    EXPECT_TRUE(m_mem->jit_code_pages_overlap(kBase + 0x1FFF, 2));
    // Range ends one byte short of it.
    EXPECT_FALSE(m_mem->jit_code_pages_overlap(kBase + 0x1FFE, 1));
}

TEST_F(JitCodePageScanTest, SparseHighPagesUseTheFirstInRangeCandidate) {
    constexpr GuestAddr high = 0x0000'0002'4000'0000ull;
    m_mem->retain_jit_code_page(high - 0x2000);
    m_mem->retain_jit_code_page(high + 0x2000);
    m_mem->retain_jit_code_page(high + 0x10000);

    EXPECT_FALSE(m_mem->jit_code_pages_overlap(high, 0x1000));
    EXPECT_TRUE(m_mem->jit_code_pages_overlap(high + 0x1000, 0x3000));
    EXPECT_FALSE(m_mem->jit_code_pages_overlap(high + 0x3000, 0xD000));
    EXPECT_TRUE(m_mem->jit_code_pages_overlap(high + 0x10000, 1));
}

TEST_F(JitCodePageScanTest, HighRangeQueryFindsAPageInEveryHostPagePosition) {
    // High pages are indexed per 16 KiB host page. A range query must find a
    // retained page whichever host page (and so whichever index) holds it,
    // for ranges narrower than the index (looked up host page by host page)
    // and wider (every index visited), and never report a neighbour.
    constexpr GuestAddr high = 0x0000'0003'0000'0000ull;
    constexpr usize kSpan = 0x400000;  // 256 host pages across two 2 MiB chunks
    for (GuestAddr off = 0; off < kSpan; off += 0x3000) {
        const GuestAddr page = high + off;
        m_mem->retain_jit_code_page(page);
        EXPECT_TRUE(m_mem->jit_code_pages_overlap(high, kSpan))
            << "wide query missed 0x" << std::hex << page;
        EXPECT_TRUE(m_mem->jit_code_pages_overlap(page & ~GuestAddr{0x3FFF},
                                                  0x4000))
            << "host-page query missed 0x" << std::hex << page;
        EXPECT_TRUE(m_mem->jit_code_pages_overlap(page, 1));
        if (off >= 0x1000) {
            EXPECT_FALSE(m_mem->jit_code_pages_overlap(high, off))
                << "range ending below 0x" << std::hex << page;
        }
        EXPECT_FALSE(m_mem->jit_code_pages_overlap(page + 0x1000,
                                                   kSpan - off))
            << "range starting above 0x" << std::hex << page;
        m_mem->release_jit_code_page(page);
        EXPECT_FALSE(m_mem->jit_code_pages_overlap(high, kSpan));
    }
}

TEST_F(JitCodePageScanTest, ConcurrentRetainReleaseKeepsCountsExact) {
    // Threads retain and release overlapping pages, low and high, so the
    // same page's count is raced from several threads. Each thread finishes
    // holding one extra retain on its own page: exactly those pages must
    // still report code, and none once those are released.
    constexpr u32 kThreads = 8;
    constexpr u32 kIterations = 20000;
    constexpr usize kPages = 32;
    for (const GuestAddr base : {kBase, GuestAddr{0x0000'0005'0000'0000ull}}) {
        SCOPED_TRACE(::testing::Message() << "base 0x" << std::hex << base);
        const auto page_at = [&](usize i) { return base + i * 0x1000; };
        std::vector<std::thread> threads;
        for (u32 t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                u32 x = 0x2545F491u * (t + 1);
                for (u32 i = 0; i < kIterations; ++i) {
                    x ^= x << 13;
                    x ^= x >> 17;
                    x ^= x << 5;
                    const GuestAddr p = page_at(x % kPages);
                    m_mem->retain_jit_code_page(p);
                    m_mem->release_jit_code_page(p);
                }
                m_mem->retain_jit_code_page(page_at(t * 3));
            });
        }
        for (auto& t : threads) t.join();
        for (usize i = 0; i < kPages; ++i) {
            const bool held = i % 3 == 0 && i / 3 < kThreads;
            EXPECT_EQ(m_mem->jit_code_pages_overlap(page_at(i), 1), held)
                << "page " << i;
        }
        for (u32 t = 0; t < kThreads; ++t) {
            m_mem->release_jit_code_page(page_at(t * 3));
        }
        EXPECT_FALSE(m_mem->jit_code_pages_overlap(base, kPages * 0x1000));
    }
}

// ---------------------------------------------------------------------------
// The coarse envelope. Marking a page must widen the span *before* the map
// byte is published, or a reader can see "outside the span" for a page that is
// already marked and under-report code.
// ---------------------------------------------------------------------------

TEST_F(JitCodePageScanTest, EnvelopeWidensToCoverEveryRetainedPage) {
    EXPECT_EQ(m_mem->jit_code_span_high(), 0u) << "empty map must have hi == 0";
    m_mem->retain_jit_code_page(kBase + 0x8000);
    EXPECT_LE(m_mem->jit_code_span_low(), kBase + 0x8000);
    EXPECT_GE(m_mem->jit_code_span_high(), kBase + 0x9000);
    m_mem->retain_jit_code_page(kBase - 0x100000);
    EXPECT_LE(m_mem->jit_code_span_low(), kBase - 0x100000);
    EXPECT_GE(m_mem->jit_code_span_high(), kBase + 0x9000);
}

TEST_F(JitCodePageScanTest, EnvelopeIsNeverNarrowerThanTheMap) {
    // The span only grows, so after a release it stays wide: over-report is
    // the safe direction. What must never happen is a marked page outside it.
    m_mem->retain_jit_code_page(kBase);
    m_mem->retain_jit_code_page(kBase + 0x10000);
    m_mem->release_jit_code_page(kBase + 0x10000);
    m_mem->retain_jit_code_page(kBase + 0x20000);
    EXPECT_LE(m_mem->jit_code_span_low(), kBase);
    EXPECT_GE(m_mem->jit_code_span_high(), kBase + 0x21000);
    EXPECT_TRUE(m_mem->jit_code_pages_overlap(kBase + 0x20000, 1));
}

// ---------------------------------------------------------------------------
// The memo. A repeated identical query is answered from the previous result,
// but only while the page map is unchanged. A memo that outlives a
// retain_jit_code_page() is exactly the under-invalidation this whole
// mechanism exists to prevent.
// ---------------------------------------------------------------------------

TEST_F(JitCodePageScanTest, MemoExpiresWhenAPageIsRetained) {
    // Warm the memo with "no code in this region".
    ASSERT_FALSE(m_mem->jit_code_pages_overlap(kBase, kBigRegion));
    ASSERT_FALSE(m_mem->jit_code_pages_overlap(kBase, kBigRegion));
    m_mem->retain_jit_code_page(kBase + 0x40000);
    EXPECT_TRUE(m_mem->jit_code_pages_overlap(kBase, kBigRegion))
        << "a memoized 'no code' answer survived a retain_jit_code_page() -- "
           "the JIT would publish a code-bearing region into the inline store "
           "fast path and run a stale translation";
}

TEST_F(JitCodePageScanTest, MemoExpiresWhenAPageIsReleased) {
    m_mem->retain_jit_code_page(kBase + 0x40000);
    ASSERT_TRUE(m_mem->jit_code_pages_overlap(kBase, kBigRegion));
    ASSERT_TRUE(m_mem->jit_code_pages_overlap(kBase, kBigRegion));
    m_mem->release_jit_code_page(kBase + 0x40000);
    EXPECT_FALSE(m_mem->jit_code_pages_overlap(kBase, kBigRegion));
}

TEST_F(JitCodePageScanTest, MemoIsNotSharedBetweenAddressSpaces) {
    // A second GuestMemory can be built at the same host address as a
    // destroyed one; the memo key must not be a raw `this`.
    ASSERT_FALSE(m_mem->jit_code_pages_overlap(kBase, kBigRegion));
    auto other = std::make_unique<GuestMemory>();
    EXPECT_NE(other->jit_code_instance_id(), m_mem->jit_code_instance_id());
    other->retain_jit_code_page(kBase);
    EXPECT_TRUE(other->jit_code_pages_overlap(kBase, kBigRegion));
    EXPECT_FALSE(m_mem->jit_code_pages_overlap(kBase, kBigRegion));
    m_mem.reset();
    auto reborn = std::make_unique<GuestMemory>();
    reborn->retain_jit_code_page(kBase);
    EXPECT_TRUE(reborn->jit_code_pages_overlap(kBase, kBigRegion));
}

TEST_F(JitCodePageScanTest, MemoDistinguishesDifferentRanges) {
    m_mem->retain_jit_code_page(kBase + 0x40000);
    EXPECT_TRUE(m_mem->jit_code_pages_overlap(kBase, kBigRegion));
    // Same generation, different range: must not reuse the previous answer.
    EXPECT_FALSE(m_mem->jit_code_pages_overlap(kBase, 0x1000));
    EXPECT_TRUE(m_mem->jit_code_pages_overlap(kBase, kBigRegion));
    EXPECT_FALSE(m_mem->jit_code_pages_overlap(kBase + 0x41000, 0x1000));
}

TEST_F(JitCodePageScanTest, MemoSurvivesTheInterleaveTheStoreHelperIssues) {
    // One jit_mem_write* call issues TWO queries with different keys: the exact
    // store address (jit_write_requires_code_notification) and the whole
    // enclosing region (jit_publish_x64_wregion). A one-entry memo lets them
    // evict each other, so the region walk runs on every single helper store.
    // A decompressor alternating a source and a destination buffer adds two
    // more distinct region keys on top. All of them must stay resident.
    constexpr GuestAddr kSrc = kBase;
    constexpr GuestAddr kDst = kBase + 0x2000'0000;
    constexpr usize kBuf = 0x100000;
    m_mem->retain_jit_code_page(kSrc + kBuf - 0x1000);
    m_mem->retain_jit_code_page(kDst + kBuf - 0x1000);

    // Warm both region answers.
    ASSERT_TRUE(m_mem->jit_code_pages_overlap(kSrc, kBuf));
    ASSERT_TRUE(m_mem->jit_code_pages_overlap(kDst, kBuf));

    auto time_ns_per_iteration = [&](int iters) {
        volatile bool sink = false;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) {
            // Interleaved exactly as the helper does it, walking the store
            // address forward so the per-store query key is never repeated.
            const GuestAddr store = kDst + static_cast<GuestAddr>(i & 0xFFFF) * 4;
            sink = m_mem->jit_code_pages_overlap(store, 4);
            sink = m_mem->jit_code_pages_overlap(kDst, kBuf);
            sink = m_mem->jit_code_pages_overlap(kSrc, kBuf);
        }
        const auto t1 = std::chrono::steady_clock::now();
        (void)sink;
        return std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
    };

    constexpr int kIters = 100000;
    const double interleaved = time_ns_per_iteration(kIters);
    std::printf("[measure] jit_code_pages_overlap, helper interleave: "
                "%.2f ns per 3-query iteration\n", interleaved);

    // Correctness is what actually matters; assert it under the interleave.
    EXPECT_TRUE(m_mem->jit_code_pages_overlap(kSrc, kBuf));
    EXPECT_TRUE(m_mem->jit_code_pages_overlap(kDst, kBuf));
    EXPECT_FALSE(m_mem->jit_code_pages_overlap(kDst, 4));

    // Three memoized/O(1) answers. A one-entry memo turns the two 1 MiB region
    // queries back into 256-page walks, which is orders of magnitude slower.
    // Measured: ~12 ns for the three queries when all three answers stay
    // resident, ~436 ns when they collide into one slot (256-page walks).
    // 100 ns leaves room for a loaded machine and still fails loudly on a
    // regression to a single effective slot.
    EXPECT_LT(interleaved, 100.0)
        << "the interleaved helper query pattern re-walks the region: the memo "
           "is single-entry again, its index collides the region keys, or the "
           "per-store query is evicting the region answers";
}

TEST_F(JitCodePageScanTest, InterleavedMemoStillExpiresOnAPageChange) {
    // The dangerous case the extra memo entries create: a NEGATIVE answer for
    // one region cached while a *different* region is being queried, then the
    // first region gains a code page. Under-reporting here is what lets the
    // JIT publish a code-bearing region into the inline store table.
    constexpr GuestAddr kSrc = kBase;
    constexpr GuestAddr kDst = kBase + 0x2000'0000;
    constexpr usize kBuf = 0x100000;

    ASSERT_FALSE(m_mem->jit_code_pages_overlap(kSrc, kBuf));
    ASSERT_FALSE(m_mem->jit_code_pages_overlap(kDst, kBuf));
    // Re-ask so both answers are definitely resident in distinct slots.
    ASSERT_FALSE(m_mem->jit_code_pages_overlap(kSrc, kBuf));
    ASSERT_FALSE(m_mem->jit_code_pages_overlap(kDst, kBuf));

    m_mem->retain_jit_code_page(kSrc + 0x40000);
    EXPECT_TRUE(m_mem->jit_code_pages_overlap(kSrc, kBuf))
        << "a memoized 'no code' answer for one region survived a retain in "
           "that region while another region's answer was also cached";
    EXPECT_FALSE(m_mem->jit_code_pages_overlap(kDst, kBuf));

    m_mem->retain_jit_code_page(kDst + 0x40000);
    EXPECT_TRUE(m_mem->jit_code_pages_overlap(kDst, kBuf));
    EXPECT_TRUE(m_mem->jit_code_pages_overlap(kSrc, kBuf));

    m_mem->release_jit_code_page(kSrc + 0x40000);
    EXPECT_FALSE(m_mem->jit_code_pages_overlap(kSrc, kBuf));
    EXPECT_TRUE(m_mem->jit_code_pages_overlap(kDst, kBuf));
}

TEST_F(JitCodePageScanTest, SinglePageQueryIsExactAtEveryPageBoundary) {
    // Single-page queries bypass the memo and go straight to the scan. Pin the
    // boundary arithmetic that decides "single page", including a size that
    // exactly fills a page and one that straddles two.
    const GuestAddr page = kBase + 0x40000;
    m_mem->retain_jit_code_page(page);

    EXPECT_TRUE(m_mem->jit_code_pages_overlap(page, 1));
    EXPECT_TRUE(m_mem->jit_code_pages_overlap(page + 0xFFF, 1));
    EXPECT_TRUE(m_mem->jit_code_pages_overlap(page, 0x1000));
    EXPECT_FALSE(m_mem->jit_code_pages_overlap(page - 1, 1));
    EXPECT_FALSE(m_mem->jit_code_pages_overlap(page + 0x1000, 1));
    // Straddles into the code page from the page below: must still report.
    EXPECT_TRUE(m_mem->jit_code_pages_overlap(page - 2, 4));
    // Straddles out of the code page into the page above.
    EXPECT_TRUE(m_mem->jit_code_pages_overlap(page + 0xFFE, 4));
    // Two clean pages either side, no straddle into the code page.
    EXPECT_FALSE(m_mem->jit_code_pages_overlap(page - 0x1000, 0x1000));
    EXPECT_FALSE(m_mem->jit_code_pages_overlap(page + 0x1000, 0x1000));
}

// ---------------------------------------------------------------------------
// Measurement. Not an assertion on wall time (that is a flaky test) but on the
// shape: the per-call cost of the repeated whole-region query the store
// helpers actually issue must not grow with the size of the region.
// ---------------------------------------------------------------------------

TEST_F(JitCodePageScanTest, RepeatedWholeRegionQueryCostIsSizeIndependent) {
    auto time_ns_per_call = [&](usize size, int iters) {
        // Volatile sink so the call cannot be optimised away.
        volatile bool sink = false;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) {
            sink = m_mem->jit_code_pages_overlap(kBase, size);
        }
        const auto t1 = std::chrono::steady_clock::now();
        (void)sink;
        return std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
    };

    constexpr int kIters = 200000;
    // The expensive shape: a region holding compiled code, so it can never be
    // published and every store re-asks. Mark a page at the very end so a scan
    // has to walk the whole thing before answering.
    m_mem->retain_jit_code_page(kBase + kBigRegion - 0x1000);

    const double small = time_ns_per_call(0x10000, kIters);
    const double big = time_ns_per_call(kBigRegion, kIters);
    std::printf("[measure] jit_code_pages_overlap, code-bearing region: "
                "64KiB %.2f ns/call, 256MiB %.2f ns/call\n", small, big);

    // A linear/chunk walk of 256 MiB is ~128 chunk steps; a memo is one
    // compare. 20x is a wide margin that still fails loudly on a regression.
    EXPECT_LT(big, small * 20.0 + 50.0)
        << "the whole-region query scales with region size again: the repeat "
           "memo in jit_code_pages_overlap() is gone or never hits";
}

TEST_F(JitCodePageScanTest, CodeFreeRegionQueryCostIsSizeIndependent) {
    auto time_ns_per_call = [&](usize size, int iters) {
        volatile bool sink = false;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) {
            sink = m_mem->jit_code_pages_overlap(kBase, size);
        }
        const auto t1 = std::chrono::steady_clock::now();
        (void)sink;
        return std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
    };

    constexpr int kIters = 200000;
    // Code exists, but far away: the envelope alone should reject.
    m_mem->retain_jit_code_page(kBase + kBigRegion + 0x100000);

    const double small = time_ns_per_call(0x10000, kIters);
    const double big = time_ns_per_call(kBigRegion, kIters);
    std::printf("[measure] jit_code_pages_overlap, code-free region: "
                "64KiB %.2f ns/call, 256MiB %.2f ns/call\n", small, big);

    EXPECT_LT(big, small * 20.0 + 50.0)
        << "a code-free whole-region query still scales with region size: the "
           "coarse span envelope is gone";
}
