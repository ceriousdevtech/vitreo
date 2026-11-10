// SPDX-License-Identifier: LGPL-2.1-only
// Exercise the shared main/callback dispatch boundary after a compile wait
// crosses an executable write. Fallback must use current bytes, not old IR.
#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <vector>
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "runtime/execution/block_decoder.hpp"
#include "runtime/execution/guest_run_loop_internal.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cli;

namespace {
struct MemoMode {
    bool old = JitCompileMemo::s_compiled_memo_enabled.exchange(true);
    MemoMode() { JitCompileMemo::clear_compile_memo(); reset_decode_memo(); }
    ~MemoMode() {
        JitCompileMemo::clear_compile_memo(); reset_decode_memo();
        JitCompileMemo::s_compiled_memo_enabled.store(old);
    }
};

void changed_while_waiting(bool wide, bool different_shape) {
    MemoMode mode;
    GuestMemory memory;
    constexpr GuestAddr base = 0x1000;
    ASSERT_TRUE(memory.map_region(base, 0x1000, MemoryProtection::ReadWriteExecute));
    const u8 original[] = {0xB8,1,0,0,0,0xC3};
    ASSERT_TRUE(memory.write_bytes(base, original, sizeof(original)));
    auto block = decode_basic_block(memory, base, wide);
    JitCompiler seed(memory);
    ASSERT_NE(seed.compile(block), nullptr);
    JitCompileMemo::CompiledKey key{};
    bool found = false;
    {
        auto& shard = JitCompileMemo::shard_for(base);
        std::shared_lock<JitMemoLock> lock(shard.lock);
        shard.compiled.at(base).for_each([&](const auto& e) { key=e.key; found=true; });
    }
    ASSERT_TRUE(found);
    JitCompileMemo::clear_compile_memo();
    BlockCache cache(16);
    cache.bind_guest_memory(&memory);
    auto* entry = cache.insert(std::move(block));
    JitCompiler compiler(memory);
    // Destruction order releases the claim before jthread's safety join on
    // any assertion exit. Normal completion releases and joins explicitly.
    std::jthread worker;
    auto leader = std::make_unique<JitCompileMemo::CompilationClaim>();
    leader->acquire(memory, key, entry->instruction_count, memory.executable_generation());
    ASSERT_TRUE(leader->owns_claim());
    DispatchCompileResult result;
    worker = std::jthread([&] { result=compile_and_publish_dispatch_block(*entry, compiler); });
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (JitCompileMemo::compile_waiters_for_test()==0 &&
           std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
    const bool waited = JitCompileMemo::compile_waiters_for_test()!=0;
    const std::vector<u8> replacement = different_shape ? std::vector<u8>{0x31,0xC0,0xC3}
                                                      : std::vector<u8>{0xB8,9,0,0,0,0xC3};
    const bool wrote = memory.write_bytes(base, replacement.data(), replacement.size()).is_ok();
    leader.reset();
    worker.join();
    ASSERT_TRUE(waited);
    ASSERT_TRUE(wrote);
    ASSERT_EQ(result.status, DispatchCompileStatus::Deferred);
    EXPECT_FALSE(entry->compile_failed);
    EXPECT_EQ(entry->compiled_code, nullptr);
    EXPECT_TRUE(entry->instructions.empty());
    const bool decoded = ensure_block_decoded(memory, cache, *entry);
    EXPECT_EQ(decoded, !different_shape);
    if (different_shape) EXPECT_EQ(cache.lookup(base), nullptr);
    else ASSERT_EQ(entry->instructions[0].operands[1].imm, 9u);
    CpuState cpu;
    if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
    cpu.rip=base; cpu.gpr.rax=0x77;
    Interpreter interpreter(cpu,memory);
    EXPECT_EQ(interpreter.step(), ExecResult::Continue);
    EXPECT_EQ(cpu.gpr.rax, different_shape ? 0u : 9u);
}
}

TEST(CompileDispatch, WaitedWriteRefreshesSameShapeDecodeBeforeFallback) {
    if (!environment_flag_enabled("VITREO_JIT_COALESCE_COMPILES", false)) GTEST_SKIP();
    for (const bool wide : {false,true}) changed_while_waiting(wide,false);
}
TEST(CompileDispatch, WaitedWriteInvalidatesChangedShapeBeforeSingleStep) {
    if (!environment_flag_enabled("VITREO_JIT_COALESCE_COMPILES", false)) GTEST_SKIP();
    for (const bool wide : {false,true}) changed_while_waiting(wide,true);
}
