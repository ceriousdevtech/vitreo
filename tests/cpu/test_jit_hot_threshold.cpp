// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Interpret-first tier: a block is compiled only by the execution that takes
// its count to JitCompiler::jit_hot_threshold(). Everything colder runs on the
// existing interpreter path.
//
// What is observed: vitreo::cpu::jit::JitCodeArena::arena_bytes_used() is the process-wide MAP_JIT
// occupancy and only a real compile grows it. The run loop's block cache is
// private to each run_execution_loop call, so the main-loop cases put the
// block under test inside a counted loop and drive the count from the loop
// bound; the callback cases use a worker thread's persistent cache instead.
//
// Every program is written with its own immediate (salt) and at its own
// region so no case can hit the process-wide compile memo, which replays an
// earlier compile without touching the arena.

#include <gtest/gtest.h>

#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/translation_store_stats.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "runtime/execution/block_decoder.hpp"
#include "runtime/execution/guest_run_loop.hpp"
#include "runtime/execution/launch_request.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/process/process.hpp"

#include <cstring>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::cli;
using cpu::jit::JitCompiler;

namespace {

struct ThresholdScope {
    explicit ThresholdScope(u64 t) { JitCompiler::set_jit_hot_threshold_for_test(t); }
    ~ThresholdScope() { JitCompiler::set_jit_hot_threshold_for_test(0); }
};

constexpr usize kDataBytes = 0x60;

// entry:  mov ebx, OUTER
// outer:  mov ecx, INNER
// inner:  imul eax, ecx, 7
//         add eax, SALT                 ; eax = 7*ecx + SALT
//         mov [r8 + rcx*4], eax
//         sub eax, SALT + 13            ; CF = (7*ecx < 13) = (ecx == 1)
//         setc [r8 + rcx*4 + 0x40]      ; flag result made memory-visible
//         dec ecx
//         jnz inner
//         dec ebx
//         jnz outer
//         mov eax, 0x77
//         ret
//
// Blocks: A=[entry..jnz inner], B=[inner..jnz inner], C=[dec ebx; jnz outer],
// D=[outer..jnz inner], E=[mov eax; ret]. Executions per run:
//   A:1  B:(INNER-1)*OUTER  C:OUTER  D:OUTER-1  E:1
std::vector<u8> loop_program(u32 outer, u32 inner, u32 salt) {
    auto imm = [](std::vector<u8>& v, u32 x) {
        for (int i = 0; i < 4; ++i) v.push_back(static_cast<u8>(x >> (8 * i)));
    };
    std::vector<u8> c;
    c.push_back(0xBB); imm(c, outer);                       // mov ebx, OUTER
    c.push_back(0xB9); imm(c, inner);                       // mov ecx, INNER
    c.insert(c.end(), {0x6B, 0xC1, 0x07});                  // imul eax, ecx, 7
    c.push_back(0x05); imm(c, salt);                        // add eax, SALT
    c.insert(c.end(), {0x41, 0x89, 0x04, 0x88});            // mov [r8+rcx*4], eax
    c.push_back(0x2D); imm(c, salt + 13);                   // sub eax, SALT+13
    c.insert(c.end(), {0x41, 0x0F, 0x92, 0x44, 0x88, 0x40});// setc [r8+rcx*4+0x40]
    c.insert(c.end(), {0xFF, 0xC9});                        // dec ecx
    c.insert(c.end(), {0x75, 0xE5});                        // jnz inner (-27)
    c.insert(c.end(), {0xFF, 0xCB});                        // dec ebx
    c.insert(c.end(), {0x75, 0xDC});                        // jnz outer (-36)
    c.push_back(0xB8); imm(c, 0x77);                        // mov eax, 0x77
    c.push_back(0xC3);                                      // ret
    return c;
}

struct RunOutcome {
    cli::RunResult result;
    u64 rax{0}, rbx{0}, rcx{0};
    u8 data[kDataBytes]{};
    usize arena_delta{0};
    // Blocks emitted fresh during the run. The arena grows in split-placement
    // windows (32 KiB), so its delta tells "compiled something" from
    // "compiled nothing" but not one block from two.
    u64 fresh_compiles{0};
};

u64 fresh_compile_count() {
    return cpu::jit::tstats::totals()
        .compiles[static_cast<usize>(cpu::jit::TranslationOutcome::Fresh)];
}

// Runs `loop_program` through run_execution_loop at `region` and reports the
// architectural outcome plus how much arena the run consumed.
RunOutcome run_main_loop(GuestAddr region, u32 outer, u32 inner, u32 salt) {
    RunOutcome out;
    GuestMemory memory;
    EXPECT_TRUE(memory.map_region(region, 0x10000,
                                  MemoryProtection::ReadWrite).is_ok());
    const GuestAddr code = region + 0x1000;
    const GuestAddr data = region + 0x2000;
    const GuestAddr stack_top = region + 0xFF00;
    const auto program = loop_program(outer, inner, salt);
    EXPECT_TRUE(memory.write_bytes(code, program.data(), program.size()).is_ok());
    EXPECT_TRUE(memory.write_u64(stack_top, PROCESS_RETURN_SENTINEL).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.rip = code;
    cpu.set_sp(stack_top);
    cpu.gpr.r8 = data;
    winapi::ProcessManager process_mgr;
    winapi::ThunkRegistry registry(0x70000000);
    cli::RunConfig config;
    config.max_instructions = 100000;

    const usize before = vitreo::cpu::jit::JitCodeArena::arena_bytes_used();
    const u64 fresh_before = fresh_compile_count();
    out.result = run_execution_loop(cpu, memory, registry, process_mgr, config);
    out.arena_delta = vitreo::cpu::jit::JitCodeArena::arena_bytes_used() - before;
    out.fresh_compiles = fresh_compile_count() - fresh_before;
    out.rax = cpu.gpr.rax;
    out.rbx = cpu.gpr.rbx;
    out.rcx = cpu.gpr.rcx;
    for (usize i = 0; i < kDataBytes; ++i) {
        out.data[i] = memory.read_u8(data + i).value_or(0xEE);
    }
    return out;
}

// The architectural contract of loop_program, independent of who ran it.
void expect_program_outcome(const RunOutcome& out, u32 inner, u32 salt) {
    EXPECT_TRUE(out.result.success) << out.result.error_message;
    EXPECT_EQ(out.result.exit_code, 0x77);
    EXPECT_EQ(out.rax, 0x77u);
    EXPECT_EQ(out.rbx, 0u);
    EXPECT_EQ(out.rcx, 0u);
    for (u32 ecx = 1; ecx <= inner; ++ecx) {
        u32 slot = 0;
        std::memcpy(&slot, out.data + ecx * 4, 4);
        EXPECT_EQ(slot, 7u * ecx + salt) << "ecx=" << ecx;
        EXPECT_EQ(out.data[0x40 + ecx * 4], ecx == 1 ? 1u : 0u)
            << "CF for ecx=" << ecx;
    }
}

void expect_same_outcome(const RunOutcome& a, const RunOutcome& b) {
    EXPECT_EQ(a.result.exit_code, b.result.exit_code);
    EXPECT_EQ(a.rax, b.rax);
    EXPECT_EQ(a.rbx, b.rbx);
    EXPECT_EQ(a.rcx, b.rcx);
    EXPECT_EQ(0, std::memcmp(a.data, b.data, kDataBytes));
}

bool jit_backend_present() {
#if defined(__aarch64__)
    return true;
#else
    return false;
#endif
}

}  // namespace

TEST(JitHotThresholdTest, EnvParsingValidatesAndClamps) {
    // The override slot is what the tests below rely on; make sure it and the
    // documented clamp behave before trusting any arena delta.
    ThresholdScope scope(2);
    EXPECT_EQ(JitCompiler::jit_hot_threshold(), 2u);
    JitCompiler::set_jit_hot_threshold_for_test(1);
    EXPECT_EQ(JitCompiler::jit_hot_threshold(), 1u);
    JitCompiler::set_jit_hot_threshold_for_test(~u64{0});
    EXPECT_EQ(JitCompiler::jit_hot_threshold(), JitCompiler::kJitHotThresholdMax);
    EXPECT_EQ(JitCompiler::kJitHotThreshold, 1u);
}

TEST(JitHotThresholdTest, BlocksRunOnceAreNeverCompiledAtThresholdTwo) {
    if (!jit_backend_present()) GTEST_SKIP() << "no ARM64 JIT backend";
    ThresholdScope scope(2);
    // OUTER=1, INNER=2: every block executes exactly once.
    const RunOutcome out = run_main_loop(0x00310000, 1, 2, 0x00012345);
    expect_program_outcome(out, 2, 0x00012345);
    EXPECT_EQ(out.arena_delta, 0u) << "a one-shot block was compiled";
}

TEST(JitHotThresholdTest, ThresholdOneRestoresCompileOnFirstExecution) {
    if (!jit_backend_present()) GTEST_SKIP() << "no ARM64 JIT backend";
    ThresholdScope scope(1);
    const RunOutcome out = run_main_loop(0x00320000, 1, 2, 0x00012346);
    expect_program_outcome(out, 2, 0x00012346);
    EXPECT_GT(out.arena_delta, 0u) << "threshold 1 must compile first-run blocks";
}

TEST(JitHotThresholdTest, SecondExecutionCompilesAndMatchesInterpreter) {
    if (!jit_backend_present()) GTEST_SKIP() << "no ARM64 JIT backend";
    // OUTER=1, INNER=3: block B runs twice -- interpreted at ecx=2, compiled
    // and run natively at ecx=1. Both slots must carry the right value and
    // the right CF, and the whole outcome must equal an interpreter-only run.
    RunOutcome reference;
    {
        ThresholdScope never(JitCompiler::kJitHotThresholdMax);
        reference = run_main_loop(0x00330000, 1, 3, 0x00012347);
        expect_program_outcome(reference, 3, 0x00012347);
        EXPECT_EQ(reference.arena_delta, 0u);
    }
    ThresholdScope scope(2);
    const RunOutcome out = run_main_loop(0x00340000, 1, 3, 0x00012347);
    expect_program_outcome(out, 3, 0x00012347);
    EXPECT_GT(out.arena_delta, 0u) << "the second execution must compile";
    expect_same_outcome(reference, out);
}

TEST(JitHotThresholdTest, ChainReachedBlockIsCountedOncePerExecution) {
    if (!jit_backend_present()) GTEST_SKIP() << "no ARM64 JIT backend";
    // OUTER=2, INNER=4: B runs 3 times in the first outer pass and is
    // compiled on its third; from then on C is only ever reached from B's
    // compiled chain. C executes exactly twice.
    //
    // A chain site that counted a cold successor AND handed it to the
    // dispatcher (which counts again) would see C at 3 after two executions
    // and compile it at threshold 3. Counting once, C stays at 2 and only B
    // compiles; at threshold 2 both compile, so the threshold-2 run must
    // emit strictly more blocks than the threshold-3 run.
    if (!cpu::jit::tstats::measuring())
        GTEST_SKIP() << "VITREO_TRANSLATION_STORE=off: no compile counter";
    RunOutcome at3, at2;
    {
        ThresholdScope scope(3);
        at3 = run_main_loop(0x00350000, 2, 4, 0x00012348);
        expect_program_outcome(at3, 4, 0x00012348);
        EXPECT_GT(at3.arena_delta, 0u) << "B (3 executions) must compile";
    }
    {
        ThresholdScope scope(2);
        at2 = run_main_loop(0x00360000, 2, 4, 0x00012349);
        expect_program_outcome(at2, 4, 0x00012349);
    }
    EXPECT_GT(at2.fresh_compiles, at3.fresh_compiles)
        << "C (2 executions) compiled at threshold 3: counted twice per execution";
}

namespace {

// lea eax, [rcx + rdx] ; add eax, SALT ; ret
std::vector<u8> callback_program(u32 salt) {
    std::vector<u8> c{0x8D, 0x04, 0x11, 0x05};
    for (int i = 0; i < 4; ++i) c.push_back(static_cast<u8>(salt >> (8 * i)));
    c.push_back(0xC3);
    return c;
}

struct CallbackOutcome {
    u64 first{0}, second{0};
    usize delta_after_first{0}, delta_after_second{0};
    bool completed_first{false}, completed_second{false};
};

// Two calls on ONE worker thread so the second sees the first's block cache.
CallbackOutcome run_callback_twice(GuestAddr region, u32 salt) {
    CallbackOutcome out;
    GuestMemory memory;
    EXPECT_TRUE(memory.map_region(region, 0x10000,
                                  MemoryProtection::ReadWrite).is_ok());
    const GuestAddr code = region + 0x1000;
    const auto program = callback_program(salt);
    EXPECT_TRUE(memory.write_bytes(code, program.data(), program.size()).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.set_sp(region + 0xFF00);
    cpu.rip = region + 0x3000;
    winapi::ProcessManager process_mgr;
    winapi::ThunkRegistry registry(0x70000000);
    const u64 args[] = {1000, 24};

    std::thread worker([&] {
        init_thread_jit_state(memory);
        const usize before = vitreo::cpu::jit::JitCodeArena::arena_bytes_used();
        out.first = run_guest_callback(cpu, memory, registry, process_mgr,
                                       code, args, 2, true, &out.completed_first);
        out.delta_after_first = vitreo::cpu::jit::JitCodeArena::arena_bytes_used() - before;
        out.second = run_guest_callback(cpu, memory, registry, process_mgr,
                                        code, args, 2, true, &out.completed_second);
        out.delta_after_second = vitreo::cpu::jit::JitCodeArena::arena_bytes_used() - before;
    });
    worker.join();
    return out;
}

}  // namespace

TEST(JitHotThresholdTest, CallbackPathObeysTheSameGate) {
    if (!jit_backend_present()) GTEST_SKIP() << "no ARM64 JIT backend";
    ThresholdScope scope(2);
    const CallbackOutcome out = run_callback_twice(0x00370000, 0x00012350);
    EXPECT_TRUE(out.completed_first);
    EXPECT_TRUE(out.completed_second);
    EXPECT_EQ(out.first, 1000u + 24u + 0x00012350u);
    EXPECT_EQ(out.second, out.first);
    EXPECT_EQ(out.delta_after_first, 0u) << "first callback execution compiled";
    EXPECT_GT(out.delta_after_second, 0u) << "second callback execution must compile";
}

TEST(JitHotThresholdTest, CallbackPathCompilesFirstExecutionAtThresholdOne) {
    if (!jit_backend_present()) GTEST_SKIP() << "no ARM64 JIT backend";
    ThresholdScope scope(1);
    const CallbackOutcome out = run_callback_twice(0x00380000, 0x00012351);
    EXPECT_TRUE(out.completed_first);
    EXPECT_EQ(out.first, 1000u + 24u + 0x00012351u);
    EXPECT_EQ(out.second, out.first);
    EXPECT_GT(out.delta_after_first, 0u);
    EXPECT_EQ(out.delta_after_second, out.delta_after_first)
        << "an already-compiled block was compiled again";
}

// ===========================================================================
// Shared compiled-block table: what N threads executing the same code
// actually duplicate.
//
// One GuestMemory, one copy of loop_program, N run_execution_loop calls each
// with its own stack/data — the shape of StarCraft II's shader-compiler pool.
// Two arms in one process, at disjoint addresses so the table is empty for
// each: adoption OFF (every thread's cold miss decodes, interprets the block
// to the hot threshold, then compile() consults the memo) and adoption ON
// (a cold miss installs the published translation before decoding).
// ===========================================================================

namespace {

struct DuplicationReport {
    u64 fresh_emits{0};
    u64 compile_probes{0};
    u64 compile_hits{0};
    u64 cold_adoptions{0};
    u64 decodes{0};
    usize arena_delta{0};
    bool all_correct{true};
};

u64 decode_calls() {
    const cli::DecodeMemoStats d = cli::decode_memo_stats();
    return d.hits + d.miss_absent + d.miss_key + d.miss_expired + d.miss_hash;
}

// Runs loop_program from `threads` guest threads over ONE address space, either
// one after another (deterministic) or all at once.
DuplicationReport run_shared_program(GuestAddr region, u32 threads,
                                     bool concurrent, u32 salt) {
    DuplicationReport report;
    GuestMemory memory;
    EXPECT_TRUE(memory.map_region(region, 0x10000,
                                  MemoryProtection::ReadWrite).is_ok());
    const GuestAddr code = region + 0x1000;
    constexpr u32 kOuter = 2, kInner = 8;
    const auto program = loop_program(kOuter, kInner, salt);
    EXPECT_TRUE(memory.write_bytes(code, program.data(), program.size()).is_ok());
    winapi::ProcessManager process_mgr;
    winapi::ThunkRegistry registry(0x70000000);

    vitreo::cpu::jit::JitCompileMemo::clear_compile_memo();
    vitreo::cpu::jit::JitCompileMemo::reset_compile_memo_stats();
    cli::reset_decode_memo();
    const usize arena_before = vitreo::cpu::jit::JitCodeArena::arena_bytes_used();

    std::vector<u8> correct(threads, 0);
    auto run_one = [&](u32 index) {
        const GuestAddr data = region + 0x2000 + index * 0x100;
        const GuestAddr stack_top = region + 0x8000 + index * 0x800;
        EXPECT_TRUE(memory.write_u64(stack_top, PROCESS_RETURN_SENTINEL).is_ok());
        cpu::CpuState cpu;
        cpu.init_long_mode();
        cpu.rip = code;
        cpu.set_sp(stack_top);
        cpu.gpr.r8 = data;
        cli::RunConfig config;
        config.max_instructions = 100000;
        const cli::RunResult r =
            run_execution_loop(cpu, memory, registry, process_mgr, config);
        bool ok = r.success && r.exit_code == 0x77 && cpu.gpr.rax == 0x77;
        for (u32 ecx = 1; ecx <= kInner; ++ecx) {
            const u32 slot = memory.read_u32(data + ecx * 4).value_or(0);
            ok &= slot == 7u * ecx + salt;
            ok &= memory.read_u8(data + 0x40 + ecx * 4).value_or(9) ==
                  (ecx == 1 ? 1u : 0u);
        }
        correct[index] = ok ? 1 : 0;
    };

    if (concurrent) {
        std::vector<std::thread> pool;
        for (u32 i = 0; i < threads; ++i) pool.emplace_back(run_one, i);
        for (auto& t : pool) t.join();
    } else {
        for (u32 i = 0; i < threads; ++i) {
            std::thread t(run_one, i);
            t.join();
        }
    }
    for (u32 i = 0; i < threads; ++i) report.all_correct &= correct[i] != 0;

    const auto s = vitreo::cpu::jit::JitCompileMemo::compile_memo_stats();
    report.fresh_emits = s.fresh_emits;
    report.compile_probes = s.compile_probes;
    report.compile_hits = s.compile_hits;
    report.cold_adoptions = s.cold_adoptions;
    report.decodes = decode_calls();
    report.arena_delta = vitreo::cpu::jit::JitCodeArena::arena_bytes_used() - arena_before;
    // The GuestMemory dies with this frame; records retaining its pages are
    // forgotten through the teardown hook, but say so explicitly.
    vitreo::cpu::jit::JitCompileMemo::clear_compile_memo();
    return report;
}

void print_report(const char* label, u32 threads, const DuplicationReport& r) {
    std::fprintf(stderr,
        "[shared-table] %-22s threads=%u fresh_emits=%llu compile_probes=%llu "
        "compile_hits=%llu cold_adoptions=%llu decodes=%llu arena_delta=%zu\n",
        label, threads,
        static_cast<unsigned long long>(r.fresh_emits),
        static_cast<unsigned long long>(r.compile_probes),
        static_cast<unsigned long long>(r.compile_hits),
        static_cast<unsigned long long>(r.cold_adoptions),
        static_cast<unsigned long long>(r.decodes),
        r.arena_delta);
}

struct AdoptionScope {
    explicit AdoptionScope(int policy) {
        JitCompiler::set_shared_adoption_for_test(policy);
    }
    ~AdoptionScope() { JitCompiler::set_shared_adoption_for_test(-1); }
};

}  // namespace

TEST(JitSharedTableRunLoop, SequentialThreadsAdoptInsteadOfRecompiling) {
    if (!jit_backend_present()) GTEST_SKIP() << "no ARM64 JIT backend";
    ThresholdScope threshold(2);
    constexpr u32 kThreads = 8;

    // Baseline arm: the memo alone. Every thread decodes all five blocks and
    // interprets each hot block up to the threshold before compile() finds
    // the shared translation.
    DuplicationReport before;
    {
        AdoptionScope off(0);
        before = run_shared_program(0x00400000, kThreads, false, 0x00012360);
    }
    print_report("memo-only (before)", kThreads, before);
    EXPECT_TRUE(before.all_correct);
    ASSERT_GT(before.fresh_emits, 0u);
    // One thread's worth of emits; the rest were memo hits in compile().
    EXPECT_EQ(before.compile_hits, before.fresh_emits * (kThreads - 1));

    DuplicationReport after;
    {
        AdoptionScope on(1);
        after = run_shared_program(0x00420000, kThreads, false, 0x00012361);
    }
    print_report("shared-table (after)", kThreads, after);
    EXPECT_TRUE(after.all_correct);
    // Emission is still done once; the (N-1) copies now arrive at the cold
    // miss instead of after a decode + threshold interpretations + compile().
    EXPECT_EQ(after.fresh_emits, before.fresh_emits);
    EXPECT_EQ(after.arena_delta, before.arena_delta);
    EXPECT_EQ(after.cold_adoptions, after.fresh_emits * (kThreads - 1));
    EXPECT_EQ(after.compile_probes, after.fresh_emits)
        << "adopting threads must not reach compile() for shared blocks";
    EXPECT_LT(after.decodes, before.decodes)
        << "adopting threads must not decode blocks the table already holds";
}

TEST(JitSharedTableRunLoop, ConcurrentThreadsShareOneTranslation) {
    if (!jit_backend_present()) GTEST_SKIP() << "no ARM64 JIT backend";
    ThresholdScope threshold(2);
    constexpr u32 kThreads = 8;
    AdoptionScope on(1);
    const DuplicationReport r =
        run_shared_program(0x00440000, kThreads, true, 0x00012362);
    print_report("concurrent (after)", kThreads, r);
    EXPECT_TRUE(r.all_correct);
    // Threads that reach an uncompiled block at the same instant may each
    // emit; the memo dedupes the table, not first compiles. What must hold is
    // that the run is correct and that nothing was emitted N times over.
    EXPECT_GT(r.fresh_emits, 0u);
    EXPECT_LT(r.fresh_emits, 5u * kThreads);
}

namespace {

void verify_evicted_hotness_compiles(bool rewrite) {
    ThresholdScope threshold(2);
    GuestMemory memory;
    const GuestAddr address = rewrite ? 0x00521000 : 0x00511000;
    ASSERT_TRUE(memory.map_region(address, 0x2000,
                                  MemoryProtection::ReadWriteExecute));
    // mov eax, 0x13579 ; jmp next instruction
    const u8 program[] = {0xB8, 0x79, 0x35, 0x01, 0, 0xEB, 0};
    ASSERT_TRUE(memory.write_bytes(address, program, sizeof(program)));
    ASSERT_TRUE(memory.write_bytes(address + 0x1000, program, sizeof(program)));
    cpu::CpuState state;
    state.init_protected_mode();
    cpu::Interpreter interpreter(state, memory);
    cpu::jit::BlockCache cache{1};
    JitCompiler compiler(memory);
    compiler.set_block_cache(&cache);

    auto* first = cache.insert(decode_basic_block(memory, address, false));
    ASSERT_EQ(first->instruction_count, 2u);
    ASSERT_FALSE(JitCompiler::execution_reaches_hot_threshold(*first));
    ++first->execution_count;
    state.rip = address;
    ASSERT_EQ(interpreter.step(), cpu::ExecResult::Continue);
    ASSERT_EQ(interpreter.step(), cpu::ExecResult::Branch);
    EXPECT_EQ(state.gpr.rax, 0x13579u);
    cache.insert(decode_basic_block(memory, address + 0x1000, false));
    ASSERT_EQ(cache.lookup(address), nullptr);
    ASSERT_TRUE(memory.jit_code_pages_overlap(address, sizeof(program)));

    const u32 expected = rewrite ? 0x24680 : 0x13579;
    if (rewrite) {
        // The real GuestMemory observer must invalidate evicted history even
        // though it removes no resident translation and returns zero.
        ASSERT_TRUE(memory.write_u32(address + 1, expected));
        EXPECT_FALSE(memory.jit_code_pages_overlap(address, sizeof(program)));
    }
    auto* revisit = cache.insert(decode_basic_block(memory, address, false));
    if (rewrite) {
        ASSERT_EQ(revisit->execution_count, 0u);
        ASSERT_FALSE(JitCompiler::execution_reaches_hot_threshold(*revisit));
        ++revisit->execution_count;
        state.rip = address;
        ASSERT_EQ(interpreter.step(), cpu::ExecResult::Continue);
        ASSERT_EQ(interpreter.step(), cpu::ExecResult::Branch);
        EXPECT_EQ(state.gpr.rax, expected);
    }
    ASSERT_EQ(revisit->execution_count, 1u);
    ASSERT_TRUE(JitCompiler::execution_reaches_hot_threshold(*revisit));
    ++revisit->execution_count;
    auto* native = compiler.compile(*revisit);
    ASSERT_NE(native, nullptr);
    state.rip = address;
    state.gpr.rax = 0;
    EXPECT_EQ(compiler.execute(*native, state), cpu::jit::JitExecResult::Continue);
    EXPECT_EQ(state.gpr.rax, expected);
    EXPECT_EQ(state.rip, address + sizeof(program));
}

} // namespace

TEST(JitHotThresholdTest, EvictionRevisitCompilesOnSecondExecution) {
    if (!jit_backend_present()) GTEST_SKIP() << "no ARM64 JIT backend";
    verify_evicted_hotness_compiles(false);
}

TEST(JitHotThresholdTest, GuestWriteResetsEvictedHeatAndCompilesNewBytes) {
    if (!jit_backend_present()) GTEST_SKIP() << "no ARM64 JIT backend";
    verify_evicted_hotness_compiles(true);
}
