// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_hostcall_ring_notice.cpp
/// @brief The "capture disabled" notice is a hint, not a log stream.
///
/// `vitreo_hostcall_dump` is requested from failure paths that can fire
/// thousands of times in a single guest run (a 32-bit callback taking an
/// access violation asks for one on every fault). When capture is off the
/// dump has nothing to print, and the notice that names the enabling
/// environment variable used to be emitted once per request — 9,241 lines in
/// one measured Battle.net run, drowning every other diagnostic in the log.
///
/// The contract this pins: the notice is printed at most once per process, no
/// matter how many dumps are requested. It is deliberately not gated behind
/// VITREO_TRACE, because a single line telling the reader which knob turns
/// capture on is worth having in a default run.

#include "runtime/diagnostics/hostcall_ring.hpp"

#include <gtest/gtest.h>
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/error.hpp"

#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

/// Run `body` with stderr redirected to a temporary file and return what it
/// wrote. Uses dup/dup2 rather than freopen so the original stderr can be put
/// back exactly as it was, which matters because gtest keeps using it.
template <typename Fn>
std::string capture_stderr(Fn&& body) {
    std::fflush(stderr);
    const int saved = ::dup(STDERR_FILENO);
    EXPECT_GE(saved, 0);

    std::FILE* sink = std::tmpfile();
    EXPECT_NE(sink, nullptr);
    ::dup2(::fileno(sink), STDERR_FILENO);

    body();

    std::fflush(stderr);
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);

    std::rewind(sink);
    std::string out;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), sink)) > 0) {
        out.append(buf, n);
    }
    std::fclose(sink);
    return out;
}

std::size_t count_occurrences(const std::string& haystack,
                              const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

TEST(HostcallRingNotice, DisabledNoticeIsNotEmittedPerRequest) {
    if (vitreo::cli::hostcall_capture_enabled()) {
        GTEST_SKIP() << "VITREO_HOSTCALL_* is set, so dumps are real dumps";
    }

    // Many requests, as a faulting guest would make. The count must not track
    // the number of requests. (Another test in this binary may already have
    // consumed the one-shot notice; "at most one" is the invariant either
    // way, and the regression this guards prints one per call.)
    const std::string captured = capture_stderr([] {
        for (int i = 0; i < 50; ++i) {
            vitreo_hostcall_dump("hostcall-ring-notice-test");
        }
    });

    EXPECT_LE(count_occurrences(captured, "[hostcall-ring]"), 1u)
        << "capture-disabled notice repeated per request:\n"
        << captured;
}

TEST(HostcallRingDetail, SelectorUsesExactCaseInsensitiveLeaves) {
    using vitreo::cli::hostcall_detail_matches;
    EXPECT_TRUE(hostcall_detail_matches("kernel32.dll!ReadFile", ""));
    EXPECT_TRUE(hostcall_detail_matches("kernel32.dll!ReadFile", "ReadFile"));
    EXPECT_TRUE(hostcall_detail_matches("kernel32.dll!ReadFile", " , readfile, GetQueuedCompletionStatus, "));
    EXPECT_TRUE(hostcall_detail_matches("GetQueuedCompletionStatus", "ReadFile,GetQueuedCompletionStatus"));
    EXPECT_FALSE(hostcall_detail_matches("ReadFileEx", "ReadFile"));
    EXPECT_FALSE(hostcall_detail_matches("ReadFile", "ReadFileEx"));
    EXPECT_FALSE(hostcall_detail_matches("WriteFile", "ReadFile,GetQueuedCompletionStatus"));
    EXPECT_FALSE(hostcall_detail_matches("ReadFile", " , , "));
}

TEST(HostcallRingDetail, CompletionSnapshotsPreserveGuestWidthsAndFailureValues) {
    using namespace vitreo;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x10000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    for (bool wide : {false, true}) {
        cli::HostcallEntry entry;
        entry.is_64bit = wide;
        entry.args[1] = 0x10000;
        entry.args[2] = 0x10010;
        entry.args[3] = 0x10020;
        ASSERT_TRUE(memory.write_u64(0x10000, 0xabcdef0100000036).is_ok());
        ASSERT_TRUE(memory.write_u64(0x10010, 0x123456789abcdef0).is_ok());
        ASSERT_TRUE(memory.write_u64(0x10020, 0x23456789abcdef01).is_ok());
        for (u64 result : {0ull, 1ull}) {
            entry.eax_out = result;
            entry.gle_after = 1234;
            winapi::set_last_error(4321);
            cli::hostcall_capture_completion_outputs(entry, memory);
            EXPECT_TRUE(entry.completion_snapshot);
            EXPECT_EQ(entry.completion_readable, 7);
            EXPECT_EQ(entry.completion_outputs[0], 0x36u);
            EXPECT_EQ(entry.completion_outputs[1], wide ? 0x123456789abcdef0ull : 0x9abcdef0ull);
            EXPECT_EQ(entry.completion_outputs[2], wide ? 0x23456789abcdef01ull : 0xabcdef01ull);
            EXPECT_EQ(entry.eax_out, result);
            EXPECT_EQ(entry.gle_after, 1234u);
            EXPECT_EQ(winapi::get_last_error(), 4321u);
        }
        // Timeout/null OVERLAPPED is distinct from an inaccessible output.
        ASSERT_TRUE(memory.write_u64(0x10020, 0).is_ok());
        entry.eax_out = 0;
        cli::hostcall_capture_completion_outputs(entry, memory);
        EXPECT_EQ(entry.completion_readable, 7);
        EXPECT_EQ(entry.completion_outputs[2], 0u);
        entry.args[1] = 0;
        entry.args[2] = 0x20000; // Unmapped.
        entry.args[3] = ~0ull;
        cli::hostcall_capture_completion_outputs(entry, memory);
        EXPECT_EQ(entry.completion_readable, 0);
        EXPECT_EQ(entry.completion_outputs[0], 0u);
        EXPECT_EQ(entry.completion_outputs[1], 0u);
        EXPECT_EQ(entry.completion_outputs[2], 0u);
    }
}

} // namespace
