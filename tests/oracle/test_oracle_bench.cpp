// SPDX-License-Identifier: LGPL-2.1-only
#include "oracle/oracle_client.hpp"
#include "oracle/oracle_contract.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

using vitreo::oracle::State;
using vitreo::oracle::BenchSession;
using vitreo::oracle::contract::compare;

class OracleBenchTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!vitreo::oracle::available()) {
            GTEST_SKIP() << "oracle runner not available (requires the host x86 runner)";
        }
    }
};

TEST_F(OracleBenchTest, PersistentAddMatchesOneshot) {
    State in{};
    in.gpr[0] = 0x11111111u;
    in.gpr[1] = 0x22222222u;
    const std::vector<uint8_t> add_eax_ecx{0x01, 0xC8};

    State oneshot{};
    ASSERT_TRUE(vitreo::oracle::execute(in, add_eax_ecx, &oneshot));
    ASSERT_EQ(oneshot.status, 0u);

    BenchSession bench;
    ASSERT_TRUE(bench.load(in, add_eax_ecx));
    State once{};
    ASSERT_TRUE(bench.execute_once(&once));
    ASSERT_EQ(once.status, 0u);
    EXPECT_TRUE(compare(once, oneshot).empty()) << compare(once, oneshot);

    ASSERT_TRUE(bench.warmup(100));
    ASSERT_TRUE(bench.run(1000));
    vitreo::oracle::BenchTimes times;
    ASSERT_TRUE(bench.times(&times));
    EXPECT_EQ(times.status, 0u);
    EXPECT_EQ(times.iters, 1000u);
    EXPECT_GT(times.run_ns, 0u);
}

TEST_F(OracleBenchTest, JitWarmLoopMatchesOracle) {
    State in{};
    in.gpr[0] = 0xAAAAAAAAu;
    in.gpr[1] = 0x00000001u;
    const std::vector<uint8_t> add_eax_ecx{0x01, 0xC8};

    auto jit = vitreo::oracle::contract::bench_on_jit(in, add_eax_ecx, 10, 100);
    ASSERT_EQ(jit.status, 0u) << "JIT bench status";
    EXPECT_EQ(jit.iters, 100u);
    EXPECT_GT(jit.run_ns, 0u);

    BenchSession bench;
    ASSERT_TRUE(bench.load(in, add_eax_ecx));
    State host{};
    ASSERT_TRUE(bench.execute_once(&host));
    ASSERT_EQ(host.status, 0u);
    EXPECT_TRUE(compare(jit.out, host).empty()) << compare(jit.out, host);
}

TEST_F(OracleBenchTest, JitWarmLoop64ReturnsFinalState) {
    State in{};
    in.mode = vitreo::oracle::kMode64;
    in.gpr[0] = 2;
    in.gpr[8] = 3;
    const std::vector<uint8_t> add_rax_r8{0x4C, 0x01, 0xC0};

    auto jit = vitreo::oracle::contract::bench_on_jit_64(
        in, add_rax_r8, 10, 100);
    ASSERT_EQ(jit.status, 0u) << "x64 JIT bench status";
    EXPECT_EQ(jit.iters, 100u);
    EXPECT_GT(jit.run_ns, 0u);
    EXPECT_EQ(jit.out.mode, vitreo::oracle::kMode64);
    EXPECT_EQ(jit.out.gpr[0], 5u);
    EXPECT_EQ(jit.out.gpr[8], 3u);
    EXPECT_EQ(jit.out.bytes_run, add_rax_r8.size());
}

TEST_F(OracleBenchTest, JitWarmLoop64MeasuresEmptyRestoreControl) {
    State in{};
    in.mode = vitreo::oracle::kMode64;
    in.gpr[0] = 0x1122334455667788ull;
    in.gpr[8] = 0x8877665544332211ull;

    auto jit = vitreo::oracle::contract::bench_on_jit_64(in, {}, 10, 100);
    ASSERT_EQ(jit.status, 0u) << "empty x64 JIT bench status";
    EXPECT_EQ(jit.iters, 100u);
    EXPECT_GT(jit.run_ns, 0u);
    EXPECT_EQ(jit.compile_ns, 0u);
    EXPECT_EQ(jit.out.gpr[0], in.gpr[0]);
    EXPECT_EQ(jit.out.gpr[4], 0u);
    EXPECT_EQ(jit.out.gpr[8], in.gpr[8]);
    EXPECT_EQ(jit.out.bytes_run, 0u);
}

TEST_F(OracleBenchTest, JitWarmLoop64StopsAfterWarmupFailure) {
    State in{};
    in.mode = vitreo::oracle::kMode64;
    const std::vector<uint8_t> read_null{
        0x48, 0x31, 0xC0,  // xor rax, rax
        0x48, 0x8B, 0x00,  // mov rax, [rax]
    };

    auto jit = vitreo::oracle::contract::bench_on_jit_64(
        in, read_null, 1, 100);
    EXPECT_NE(jit.status, 0u);
    EXPECT_EQ(jit.iters, 0u);
    EXPECT_EQ(jit.run_ns, 0u);

    auto measured = vitreo::oracle::contract::bench_on_jit_64(
        in, read_null, 0, 100);
    EXPECT_NE(measured.status, 0u);
    EXPECT_EQ(measured.iters, 0u);
    EXPECT_GT(measured.run_ns, 0u);
}

TEST_F(OracleBenchTest, JitMultiBlock64DirectCallRetBalancesStack) {
    State in{};
    in.mode = vitreo::oracle::kMode64;
    in.gpr[0] = 10;
    // call callee; inc rax; jmp end; callee: add rax,2; ret
    const std::vector<uint8_t> code{
        0xE8, 0x05, 0x00, 0x00, 0x00,
        0x48, 0xFF, 0xC0,
        0xEB, 0x05,
        0x48, 0x83, 0xC0, 0x02,
        0xC3,
    };

    const auto got = vitreo::oracle::contract::bench_on_jit_64_multiblock(
        in, code, 4, 20);
    ASSERT_EQ(got.bench.status, 0u);
    EXPECT_EQ(got.bench.iters, 20u);
    EXPECT_GT(got.bench.run_ns, 0u);
    EXPECT_EQ(got.bench.out.gpr[0], 13u);
    EXPECT_EQ(got.stack_delta, 0);
    EXPECT_EQ(got.final_rsp, got.initial_rsp);
    EXPECT_EQ(got.bench.out.bytes_run, code.size());
    EXPECT_GE(got.blocks_compiled, 3u);
    // Preflight publishes all three blocks into the real cache. The timed
    // body must then stay in the linked JIT chain until its final exit.
    EXPECT_EQ(got.final_dispatches, 1u);

    const State interp = vitreo::oracle::contract::run_on_interpreter_64(
        in, code);
    ASSERT_EQ(interp.status, 0u);
    EXPECT_TRUE(compare(got.bench.out, interp).empty())
        << compare(got.bench.out, interp);
}

TEST_F(OracleBenchTest, JitMultiBlock64EmptyControlKeepsExactCounters) {
    State in{};
    in.mode = vitreo::oracle::kMode64;

    const auto got = vitreo::oracle::contract::bench_on_jit_64_multiblock(
        in, {}, 3, 12);
    ASSERT_EQ(got.bench.status, 0u);
    EXPECT_EQ(got.bench.iters, 12u);
    EXPECT_GT(got.bench.run_ns, 0u);
    EXPECT_EQ(got.stack_delta, 0);
    EXPECT_EQ(got.final_rsp, got.initial_rsp);
    EXPECT_EQ(got.blocks_compiled, 0u);
    EXPECT_EQ(got.final_dispatches, 0u);
}

TEST_F(OracleBenchTest, JitMultiBlock64IndirectCallAndJumpBalanceStack) {
    State in{};
    in.mode = vitreo::oracle::kMode64;
    in.gpr[0] = 20;
    // lea r11,[callee]; call r11; inc rax; lea r10,[end]; jmp r10;
    // callee: add rax,2; ret
    const std::vector<uint8_t> code{
        0x4C, 0x8D, 0x1D, 0x10, 0x00, 0x00, 0x00,
        0x41, 0xFF, 0xD3,
        0x48, 0xFF, 0xC0,
        0x4C, 0x8D, 0x15, 0x08, 0x00, 0x00, 0x00,
        0x41, 0xFF, 0xE2,
        0x48, 0x83, 0xC0, 0x02,
        0xC3,
    };

    const auto got = vitreo::oracle::contract::bench_on_jit_64_multiblock(
        in, code, 2, 10);
    ASSERT_EQ(got.bench.status, 0u);
    EXPECT_EQ(got.bench.out.gpr[0], 23u);
    EXPECT_EQ(got.stack_delta, 0);
    EXPECT_EQ(got.final_rsp, got.initial_rsp);
    EXPECT_EQ(got.bench.out.bytes_run, code.size());
    EXPECT_EQ(got.final_dispatches, 1u);

    const State interp = vitreo::oracle::contract::run_on_interpreter_64(
        in, code);
    ASSERT_EQ(interp.status, 0u);
    EXPECT_TRUE(compare(got.bench.out, interp).empty())
        << compare(got.bench.out, interp);
}

TEST_F(OracleBenchTest, JitMultiBlock64RejectsEscapeTarget) {
    State in{};
    in.mode = vitreo::oracle::kMode64;
    // jmp one byte beyond the submitted window
    const std::vector<uint8_t> code{0xEB, 0x01};
    const auto got = vitreo::oracle::contract::bench_on_jit_64_multiblock(
        in, code, 0, 1);
    EXPECT_EQ(got.bench.status, 5u);
    EXPECT_EQ(got.bench.iters, 0u);
}

TEST_F(OracleBenchTest, JitMultiBlock64CanonicalizesStackDeltaPerIteration) {
    State in{};
    in.mode = vitreo::oracle::kMode64;
    in.gpr[0] = 0x1122334455667788ull;
    // push rax; jmp end. Each iteration intentionally ends eight bytes low;
    // the next iteration must still begin at the owned initial RSP.
    const std::vector<uint8_t> code{0x50, 0xEB, 0x00};
    const auto got = vitreo::oracle::contract::bench_on_jit_64_multiblock(
        in, code, 3, 12);
    ASSERT_EQ(got.bench.status, 0u);
    EXPECT_EQ(got.bench.iters, 12u);
    EXPECT_EQ(got.stack_delta, -8);
    EXPECT_EQ(got.final_rsp + 8, got.initial_rsp);
}

}  // namespace
