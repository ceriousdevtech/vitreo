#include <gtest/gtest.h>

#include <graphics/common/perf_diag.hpp>

#include <cstdlib>
#include <thread>
#include <vector>

using namespace vitreo;

// These tests run in a process where VITREO_PERF_DIAG is (almost certainly)
// unset, so `enabled()` is false and every entry point must be a safe no-op.
// That disabled path is the one that ships, so it is the one worth pinning:
// a crash or a data race here would fire on every user, not just on a
// diagnostic run.

TEST(PerfDiag, DisabledByDefault) {
    // The suite is not run with VITREO_PERF_DIAG set. If someone exports it in
    // their shell the remaining expectations still hold, so only assert the
    // default when the environment actually reflects it.
    if (std::getenv("VITREO_PERF_DIAG") == nullptr) {
        EXPECT_FALSE(perf::enabled());
    }
}

TEST(PerfDiag, DisabledCallsAreSafeNoOps) {
    perf::count(perf::Counter::dd_lock);
    perf::count(perf::Counter::dd_unlock, 42);
    perf::gauge_max(perf::Counter::present_submit, 7);
    perf::record(perf::Hist::frame_interval, 16'000'000ULL);
    perf::maybe_report();
    perf::report_now();
    SUCCEED();
}

TEST(PerfDiag, SpanIsSafeWhenDisabled) {
    {
        perf::Span span(perf::Hist::pump_duration);
    }
    SUCCEED();
}

TEST(PerfDiag, ConcurrentUseDoesNotCrash) {
    // Counters are touched from the guest thread, the present dispatch queue,
    // and Metal completion handlers simultaneously. Exercise that shape.
    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int index = 0; index < 4; ++index) {
        threads.emplace_back([] {
            for (int iteration = 0; iteration < 1000; ++iteration) {
                perf::count(perf::Counter::present_submit);
                perf::record(perf::Hist::present_submit_to_done,
                             static_cast<u64>(iteration) * 1000ULL);
                perf::gauge_max(perf::Counter::present_coalesced,
                                static_cast<u64>(iteration));
                perf::maybe_report();
            }
        });
    }
    for (auto& thread : threads) thread.join();
    SUCCEED();
}

// Bucket edges are load-bearing for reading a report correctly: an off-by-one
// here silently misattributes "we missed vsync" as "we made vsync".
TEST(PerfDiag, BucketEdgesAreMonotonicAndCoverTheFrameScale) {
    for (u32 index = 1; index < perf::HIST_BUCKETS - 1; ++index) {
        EXPECT_GT(perf::HIST_EDGES_NS[index], perf::HIST_EDGES_NS[index - 1])
            << "bucket edges must be strictly increasing at " << index;
    }
    // A 60 Hz frame (16.67 ms) must not land in the same bucket as a 30 Hz
    // frame (33.3 ms), or the histogram cannot show a dropped frame at all.
    const u64 sixty_hz = 16'670'000ULL;
    const u64 thirty_hz = 33'300'000ULL;
    u32 sixty_bucket = perf::HIST_BUCKETS - 1;
    u32 thirty_bucket = perf::HIST_BUCKETS - 1;
    for (u32 index = 0; index < perf::HIST_BUCKETS - 1; ++index) {
        if (sixty_hz < perf::HIST_EDGES_NS[index] &&
            sixty_bucket == perf::HIST_BUCKETS - 1) {
            sixty_bucket = index;
        }
        if (thirty_hz < perf::HIST_EDGES_NS[index] &&
            thirty_bucket == perf::HIST_BUCKETS - 1) {
            thirty_bucket = index;
        }
    }
    EXPECT_NE(sixty_bucket, thirty_bucket);
}

// Present diagnostics must not mix sibling-process frames or treat paths as
// printf programs when retaining the historical frame-number token.
#include "graphics/common/diagnostic_path.hpp"
TEST(DiagnosticPath, ExpandsOnlyLiteralPidAndOptionalFrameTokens) {
    using vitreo::graphics::diagnostic_path;
    EXPECT_EQ(diagnostic_path("/tmp/frame-%p-%u.ppm", 42).value(), "/tmp/frame-42-%u.ppm");
    EXPECT_EQ(diagnostic_path("/tmp/frame-%p-%u.ppm", 42, 7).value(), "/tmp/frame-42-7.ppm");
    EXPECT_EQ(diagnostic_path("%p/%p/%u/%u", 42, 7).value(), "42/42/7/7");
    EXPECT_EQ(diagnostic_path("%s%n%04u%", 42, 7).value(), "%s%n%04u%");
    EXPECT_EQ(diagnostic_path("", 42).value(), "");
    EXPECT_NE(diagnostic_path("%p", 42), diagnostic_path("%p", 43));
}
TEST(DiagnosticPath, RejectsOverlongOutputWithoutTruncation) {
    using vitreo::graphics::diagnostic_path;
    EXPECT_TRUE(diagnostic_path(std::string(1023, 'x'), 42).has_value());
    EXPECT_FALSE(diagnostic_path(std::string(1024, 'x'), 42).has_value());
    EXPECT_FALSE(diagnostic_path(std::string(1022, 'x') + "%p", 42).has_value());
    EXPECT_TRUE(diagnostic_path(std::string(1022, 'x') + "%u", 42, 7).has_value());
}
