// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_gpu_watch.cpp
/// @brief The command-buffer summary gpu_watch reports when the GPU hangs.

#include <gtest/gtest.h>

#include "graphics/metal/metal_gpu_watch.hpp"

#include <string>

using namespace vitreo::graphics;

TEST(GpuWatch, SummaryListsPipelinesInOrderWithoutImmediateRepeats) {
    std::string summary;
    gpu_watch::note(summary, "pass2560x1440");
    gpu_watch::note(summary, "vs:1/fs:2");
    gpu_watch::note(summary, "vs:1/fs:2");  // a thousand draws, one entry
    gpu_watch::note(summary, "cs:3");
    gpu_watch::note(summary, "vs:1/fs:2");  // a later rebind is listed again
    gpu_watch::note(summary, nullptr);
    gpu_watch::note(summary, "");
    EXPECT_EQ(summary, "pass2560x1440 vs:1/fs:2 cs:3 vs:1/fs:2");
}

TEST(GpuWatch, SummaryIsBounded) {
    std::string summary;
    for (int i = 0; i < 10000; ++i) {
        gpu_watch::note(summary, i % 2 ? "cs:aaaaaaaaaaaaaaaa" : "cs:bbbbbbbbbbbbbbbb");
    }
    EXPECT_LE(summary.size(), 4096u + 32u);
}
