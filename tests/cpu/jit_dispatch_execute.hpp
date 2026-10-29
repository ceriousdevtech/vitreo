// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include <gtest/gtest.h>

#include "core/cpu/jit/jit_compiler.hpp"

namespace vitreo::cpu::jit::test {

/// JitCompiler::execute() as the dispatcher drives it. When any arena
/// retirement round has happened since this compiler last synchronized (a
/// compile can retire cold chunks under the soft budget), execute() purges
/// its stale pointers and reports StaleCode once without entering native
/// code; the dispatcher then simply re-enters. This retries exactly once. A
/// second StaleCode means the block itself was retired, which fails the test.
inline JitExecResult execute_like_dispatcher(JitCompiler& jit,
                                             CompiledBlock& block,
                                             CpuState& cpu) {
    const JitExecResult first = jit.execute(block, cpu);
    if (first != JitExecResult::StaleCode) return first;
    const JitExecResult second = jit.execute(block, cpu);
    EXPECT_NE(second, JitExecResult::StaleCode)
        << "compiled block at 0x" << std::hex << block.guest_addr
        << " reported StaleCode twice: its own chunk was retired";
    return second;
}

}  // namespace vitreo::cpu::jit::test
