// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Opt-in SSE helper histogram tests

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "core/cpu/jit/jit_helpers.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

TEST(JitSseHelperHistogramTest, PeriodicallyReportsHotHelperOpIds) {
    // Drive the switch directly: the environment variable is only read at
    // static init, and other tests in this binary share the histogram.
    jit_set_sse_helper_histogram_enabled(true);
    jit_reset_sse_helper_histogram();

    CpuState cpu;
    GuestMemory memory;
    cpu.sse.xmm[0].u8_[0] = 0;
    cpu.sse.xmm[1].u8_[0] = 2;

    testing::internal::CaptureStderr();
    for (u64 invocation = 0; invocation < 256; ++invocation) {
        jit_sse_op(&cpu, &memory, 74, 0, 1);
    }
    const std::string output = testing::internal::GetCapturedStderr();
    jit_set_sse_helper_histogram_enabled(false);

    EXPECT_NE(output.find("[jit-sse-hist] total=256"), std::string::npos);
    EXPECT_NE(
        output.find("[jit-sse-hist] op_id=74 count=256 share=100.0%"),
        std::string::npos);
}
