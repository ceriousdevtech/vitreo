// SPDX-License-Identifier: LGPL-2.1-only
#include "oracle/cpu_perf_catalog.hpp"
#include "oracle/cpu_perf_oracle.hpp"
#include "core/cpu/jit/hot_block_dump.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace {

using vitreo::oracle::perf::DumpBlock;
using vitreo::oracle::perf::Report;
using vitreo::oracle::perf::SnippetBench;
using vitreo::oracle::perf::attach_windows;
using vitreo::oracle::perf::bench_body;
using vitreo::oracle::perf::bench_catalog;
using vitreo::oracle::perf::bench_class_catalog;
using vitreo::oracle::perf::bench_dump;
using vitreo::oracle::perf::bench_on_windows64;
using vitreo::oracle::perf::bench_on_windows64_observed;
using vitreo::oracle::perf::bench_on_windows64_normalized;
using vitreo::oracle::perf::cpu_perf_catalog;
using vitreo::oracle::perf::loop256;
using vitreo::oracle::perf::loop256_esi;
using vitreo::oracle::perf::normalize_bench_times;
using vitreo::oracle::perf::parse_hot_dump;
using vitreo::oracle::perf::prepare_body;
using vitreo::oracle::perf::print_report;
using vitreo::oracle::perf::ratio;
using vitreo::oracle::perf::ratio_windows;
using vitreo::oracle::perf::windows_ready;
using vitreo::oracle::perf::windows64_ready;
using vitreo::oracle::perf::write_report_csv;
using vitreo::oracle::perf::write_report_json;

vitreo::oracle::perf::X64CatalogTrial x64_trial(
    uint64_t jit_run_ns, uint64_t windows_run_ns, double jit_raw,
    double jit_wrapper, double jit_net, double windows_raw,
    double windows_wrapper, double windows_net) {
    vitreo::oracle::perf::X64CatalogTrial trial;
    trial.jit.raw.status = 0;
    trial.jit.raw.run_ns = jit_run_ns;
    trial.jit.raw.iters = 100;
    trial.jit.wrapper.status = 0;
    trial.jit.wrapper.run_ns = 200;
    trial.jit.wrapper.iters = 100;
    trial.jit.ns = {jit_raw, jit_wrapper, jit_net};
    trial.windows.raw.times.run_ns = windows_run_ns;
    trial.windows.raw.times.iters = 100;
    trial.windows.wrapper.run_ns = 100;
    trial.windows.wrapper.iters = 100;
    trial.windows.ns = {windows_raw, windows_wrapper, windows_net};
    return trial;
}

class CpuPerfOracleTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!vitreo::oracle::available()) {
            GTEST_SKIP() << "oracle runner not available (requires the host x86 runner)";
        }
    }
};

TEST(CpuPerfNormalizationTest, PreservesSignedPerIterationDifference) {
    vitreo::oracle::BenchTimes raw{};
    raw.run_ns = 900;
    raw.iters = 30;
    vitreo::oracle::BenchTimes wrapper{};
    wrapper.run_ns = 400;
    wrapper.iters = 20;
    vitreo::oracle::perf::NormalizedNs ns{};
    ASSERT_TRUE(normalize_bench_times(raw, wrapper, ns));
    EXPECT_DOUBLE_EQ(ns.raw_ns_per_iter, 30.0);
    EXPECT_DOUBLE_EQ(ns.wrapper_ns_per_iter, 20.0);
    EXPECT_DOUBLE_EQ(ns.net_ns_per_iter, 10.0);

    raw.run_ns = 100;
    raw.iters = 10;
    wrapper.run_ns = 300;
    wrapper.iters = 10;
    ASSERT_TRUE(normalize_bench_times(raw, wrapper, ns));
    EXPECT_DOUBLE_EQ(ns.net_ns_per_iter, -20.0);

    wrapper.iters = 0;
    EXPECT_FALSE(normalize_bench_times(raw, wrapper, ns));
    EXPECT_DOUBLE_EQ(ns.raw_ns_per_iter, 0.0);
    EXPECT_DOUBLE_EQ(ns.wrapper_ns_per_iter, 0.0);
    EXPECT_DOUBLE_EQ(ns.net_ns_per_iter, 0.0);
}

TEST(CpuPerfNormalizationTest, HotDumpPathExpandsProcessTokens) {
    using vitreo::cpu::jit::hot_dump::expand_process_path;
    EXPECT_EQ(expand_process_path("/tmp/hot-%p.json", 42),
              "/tmp/hot-42.json");
    EXPECT_EQ(expand_process_path("%p/hot-%p.json", 9001),
              "9001/hot-9001.json");
    EXPECT_EQ(expand_process_path("/tmp/hot.json", 42), "/tmp/hot.json");
}

TEST(CpuPerfNormalizationTest, HotDumpSeparatesCodeVariantsAtOneRip) {
    namespace hot = vitreo::cpu::jit::hot_dump;
    constexpr uint64_t rip = 0x140001000ull;
    const std::array<uint8_t, 3> first{0x48, 0x01, 0xC8};
    const std::array<uint8_t, 3> second{0x48, 0x29, 0xC8};

    const int saved_state =
        hot::detail::g_state.exchange(2, std::memory_order_acq_rel);
    hot::detail::reset_for_testing();
    const uint32_t first_variant = hot::record_compile(
        rip, first.data(), static_cast<uint32_t>(first.size()), 1, false);
    hot::detail::record_exec_for_testing(rip, first_variant, 1, 32);
    const uint32_t same_variant = hot::record_compile(
        rip, first.data(), static_cast<uint32_t>(first.size()), 9, true);
    hot::detail::record_exec_for_testing(rip, same_variant, 1, 32);
    const uint32_t second_variant = hot::record_compile(
        rip, second.data(), static_cast<uint32_t>(second.size()), 2, true);
    hot::detail::record_exec_for_testing(rip, second_variant, 2, 32);
    // An older compiled block can still be executing after the SMC generation
    // is registered. Its retained token must continue charging the old bytes.
    hot::detail::record_exec_for_testing(rip, first_variant, 1, 32);
    const auto snapshot = hot::detail::snapshot_for_testing();
    hot::detail::reset_for_testing();
    hot::detail::g_state.store(saved_state, std::memory_order_release);

    ASSERT_EQ(snapshot.size(), 2u);
    EXPECT_EQ(first_variant, same_variant);
    EXPECT_NE(first_variant, second_variant);
    const auto first_row = std::find_if(
        snapshot.begin(), snapshot.end(), [first_variant](const auto& row) {
            return row.code_variant == first_variant;
        });
    const auto second_row = std::find_if(
        snapshot.begin(), snapshot.end(), [second_variant](const auto& row) {
            return row.code_variant == second_variant;
        });
    ASSERT_NE(first_row, snapshot.end());
    ASSERT_NE(second_row, snapshot.end());
    EXPECT_EQ(first_row->rip, rip);
    EXPECT_EQ(first_row->bytes,
              std::vector<uint8_t>(first.begin(), first.end()));
    EXPECT_EQ(first_row->exec_count, 96u);
    EXPECT_EQ(first_row->insn_count, 96u);
    // Recompiling identical bytes aggregates into their original provenance;
    // later metadata cannot rewrite that captured generation.
    EXPECT_EQ(first_row->native_insns, 1u);
    EXPECT_FALSE(first_row->delegated);
    EXPECT_EQ(second_row->rip, rip);
    EXPECT_EQ(second_row->bytes,
              std::vector<uint8_t>(second.begin(), second.end()));
    EXPECT_EQ(second_row->exec_count, 32u);
    EXPECT_EQ(second_row->insn_count, 64u);
    EXPECT_EQ(second_row->native_insns, 2u);
    EXPECT_TRUE(second_row->delegated);
}

TEST(CpuPerfNormalizationTest, X64StarterCatalogIsLongModeAndBounded) {
    const auto catalog = vitreo::oracle::perf::cpu_perf_x64_catalog();
    ASSERT_GE(catalog.size(), 15u);
    std::vector<std::string> names;
    size_t multiblock_rows = 0;
    for (const auto& entry : catalog) {
        EXPECT_FALSE(entry.name.empty());
        EXPECT_FALSE(entry.cls.empty());
        EXPECT_FALSE(entry.body.empty()) << entry.name;
        EXPECT_LE(entry.body.size(), 256u) << entry.name;
        EXPECT_EQ(entry.in.mode, vitreo::oracle::kMode64) << entry.name;
        EXPECT_EQ(entry.in.gpr[4], 0u) << entry.name;
        EXPECT_GE(entry.operations_per_iter, 21u) << entry.name;
        if (entry.multiblock) {
            ++multiblock_rows;
            EXPECT_EQ(entry.cls, "call-ret") << entry.name;
        }
        names.push_back(entry.name);
    }
    EXPECT_EQ(multiblock_rows, 2u);
    std::sort(names.begin(), names.end());
    EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end());
}

TEST(CpuPerfNormalizationTest, X64X87CatalogIsSyntheticBalancedAndInitialized) {
    const auto catalog = vitreo::oracle::perf::cpu_perf_x64_catalog();
    size_t x87_rows = 0;
    for (const auto& entry : catalog) {
        if (entry.cls != "x87") continue;
        ++x87_rows;
        ASSERT_GE(entry.body.size(), 2u) << entry.name;
        EXPECT_EQ(entry.in.fcw, 0x027Fu) << entry.name;
        EXPECT_EQ(entry.in.fsw, 0u) << entry.name;
        EXPECT_EQ(entry.in.ftw, 0u) << entry.name;
        EXPECT_EQ(entry.in.mode, vitreo::oracle::kMode64) << entry.name;
        EXPECT_EQ(entry.in.gpr[11], vitreo::oracle::kMemVa) << entry.name;
        EXPECT_GT(entry.operations_per_iter, 0u) << entry.name;
    }
    EXPECT_EQ(x87_rows, 5u);
}

TEST(CpuPerfNormalizationTest, X64TrialSummaryUsesPairedMedians) {
    vitreo::oracle::perf::X64CatalogRow row;
    row.trials = {
        x64_trial(1200, 400, 12.0, 2.0, 10.0, 4.0, 1.0, 3.0),
        x64_trial(2000, 1000, 20.0, 4.0, 16.0, 10.0, 2.0, 8.0),
        x64_trial(1600, 800, 16.0, 3.0, 13.0, 8.0, 2.0, 6.0),
    };
    vitreo::oracle::perf::summarize_x64_trials(row);
    ASSERT_TRUE(row.median.jit_valid);
    ASSERT_TRUE(row.median.windows_valid);
    EXPECT_DOUBLE_EQ(row.median.jit.raw_ns_per_iter, 16.0);
    EXPECT_DOUBLE_EQ(row.median.jit.net_ns_per_iter, 13.0);
    EXPECT_DOUBLE_EQ(row.median.windows.raw_ns_per_iter, 8.0);
    EXPECT_DOUBLE_EQ(row.median.windows.net_ns_per_iter, 6.0);
    ASSERT_TRUE(row.median.raw_ratio_valid);
    EXPECT_DOUBLE_EQ(row.median.raw_ratio, 2.0);
    ASSERT_TRUE(row.median.net_ratio_valid);
    EXPECT_DOUBLE_EQ(row.median.net_ratio, 13.0 / 6.0);
}

TEST(CpuPerfNormalizationTest, X64TrialSummaryRejectsUnbalancedWindowsRsp) {
    vitreo::oracle::perf::X64CatalogRow row;
    row.trials.push_back(
        x64_trial(1200, 400, 12.0, 2.0, 10.0, 4.0, 1.0, 3.0));
    row.trials[0].windows.raw.initial_rsp = 0x1000;
    row.trials[0].windows.raw.gpr[4] = 0x0FF8;

    vitreo::oracle::perf::summarize_x64_trials(row);
    EXPECT_TRUE(row.median.jit_valid);
    EXPECT_FALSE(row.median.windows_valid);
    EXPECT_FALSE(row.median.raw_ratio_valid);
    EXPECT_FALSE(row.median.net_ratio_valid);
}

TEST(CpuPerfNormalizationTest, X64JsonKeepsExactTrialsAndNullNetRatio) {
    vitreo::oracle::perf::X64CatalogRow row;
    row.name = "signed-noise";
    row.cls = "integer";
    row.operations_per_iter = 7;
    row.trials.push_back(
        x64_trial(1200, 600, 12.0, 13.25, -1.25, 6.0, 7.0, -1.0));
    vitreo::oracle::perf::summarize_x64_trials(row);
    ASSERT_TRUE(row.median.raw_ratio_valid);
    ASSERT_FALSE(row.median.net_ratio_valid);

    vitreo::oracle::perf::X64CatalogReport report;
    report.rows.push_back(row);
    report.comparable = 1;
    report.windows_comparable = 1;
    const char* dir = std::getenv("TEST_TMPDIR");
    const std::string path = std::string(dir ? dir : "/tmp") +
                             "/cpu_perf_x64_trials.json";
    std::string error;
    ASSERT_TRUE(vitreo::oracle::perf::write_x64_report_json(report, path,
                                                            error))
        << error;
    std::ifstream json(path);
    const std::string text((std::istreambuf_iterator<char>(json)),
                           std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("\"schema\":\"vitreo.cpu_perf.x64.v1\""),
              std::string::npos);
    EXPECT_EQ(text.find("\"schema\":\"vitreo.cpu_perf.v1\""),
              std::string::npos);
    EXPECT_NE(text.find("\"run_ns\":1200,\"iters\":100"),
              std::string::npos);
    EXPECT_NE(text.find("\"multiblock\":false"), std::string::npos);
    EXPECT_NE(text.find("\"stack_delta\":0"), std::string::npos);
    EXPECT_NE(text.find("\"wrapper_stack_delta\":0"), std::string::npos);
    EXPECT_NE(text.find("\"blocks_compiled\":0"), std::string::npos);
    EXPECT_NE(text.find("\"final_dispatches\":0"), std::string::npos);
    EXPECT_NE(text.find("\"net_ns_per_iter\":-1.25"),
              std::string::npos);
    EXPECT_NE(text.find("\"raw_ratio\":2"), std::string::npos);
    EXPECT_NE(text.find("\"net_ratio\":null"), std::string::npos);
}

#if defined(__aarch64__)
TEST(CpuPerfNormalizationTest, X64JitPairUsesMatchingIterations) {
    const auto catalog = vitreo::oracle::perf::cpu_perf_x64_catalog();
    ASSERT_FALSE(catalog.empty());
    vitreo::oracle::perf::X64JitNormalizedResult result{};
    std::string error;
    ASSERT_TRUE(vitreo::oracle::perf::bench_x64_jit_normalized(
        catalog.front().body, catalog.front().in, 2, 10, result, error))
        << error;
    EXPECT_EQ(result.raw.status, 0u);
    EXPECT_EQ(result.wrapper.status, 0u);
    EXPECT_EQ(result.raw.iters, result.wrapper.iters);
    EXPECT_GT(result.raw.run_ns, 0u);
    EXPECT_GT(result.wrapper.run_ns, 0u);
    EXPECT_DOUBLE_EQ(result.ns.net_ns_per_iter,
                     result.ns.raw_ns_per_iter -
                         result.ns.wrapper_ns_per_iter);
}

TEST(CpuPerfNormalizationTest, X64DirectCallRetCatalogStaysInOneDispatch) {
    const auto catalog = vitreo::oracle::perf::cpu_perf_x64_catalog();
    const auto it = std::find_if(catalog.begin(), catalog.end(),
                                 [](const auto& entry) {
        return entry.name == "x64_call_ret_direct";
    });
    ASSERT_NE(it, catalog.end());

    const auto got = vitreo::oracle::contract::bench_on_jit_64_multiblock(
        it->in, it->body, 2, 10);
    ASSERT_EQ(got.bench.status, 0u);
    EXPECT_EQ(got.bench.iters, 10u);
    EXPECT_EQ(got.stack_delta, 0);
    EXPECT_EQ(got.final_rsp, got.initial_rsp);
    EXPECT_EQ(got.final_dispatches, 1u);
}

TEST(CpuPerfNormalizationTest, X64StarterCatalogMeasuresOnJit) {
    const auto catalog = vitreo::oracle::perf::cpu_perf_x64_catalog();
    const auto report =
        vitreo::oracle::perf::bench_x64_catalog_trials(1, 10, 3);
    ASSERT_EQ(report.rows.size(), catalog.size());
    EXPECT_EQ(report.comparable, report.rows.size());
    EXPECT_EQ(report.skipped, 0u);
    for (const auto& row : report.rows) {
        ASSERT_EQ(row.trials.size(), 3u) << row.name;
        EXPECT_TRUE(row.skip.empty()) << row.name << ": " << row.skip;
        EXPECT_EQ(row.jit.raw.iters, row.jit.wrapper.iters) << row.name;
        EXPECT_TRUE(row.median.jit_valid) << row.name;
        if (row.multiblock) {
            EXPECT_EQ(row.jit.raw_stack_delta, 0) << row.name;
            EXPECT_EQ(row.jit.wrapper_stack_delta, 0) << row.name;
            EXPECT_GT(row.jit.raw_blocks_compiled, 0u) << row.name;
            EXPECT_GT(row.jit.raw_final_dispatches, 0u) << row.name;
            EXPECT_EQ(row.jit.wrapper_blocks_compiled, 0u) << row.name;
            EXPECT_EQ(row.jit.wrapper_final_dispatches, 0u) << row.name;
        }
    }
}

TEST(CpuPerfNormalizationTest, X64X87CatalogProducesDeterministicResults) {
    const auto catalog = vitreo::oracle::perf::cpu_perf_x64_catalog();
    struct Expected {
        const char* name;
        size_t offset;
        double value;
    };
    const std::array<Expected, 4> expected{{
        {"x64_x87_fld1_fadd_fstp", 0x100u, 2.0},
        {"x64_x87_fld_fadd_m64", 0x108u, 3.75},
        {"x64_x87_fld_fmul_m64", 0x110u, 3.375},
        {"x64_x87_fld_fsqrt_fstp", 0x118u, 2.0},
    }};
    for (const Expected& want : expected) {
        const auto it = std::find_if(
            catalog.begin(), catalog.end(), [&](const auto& entry) {
                return entry.name == want.name;
            });
        ASSERT_NE(it, catalog.end()) << want.name;
        vitreo::oracle::perf::X64JitNormalizedResult result{};
        std::string error;
        ASSERT_TRUE(vitreo::oracle::perf::bench_x64_jit_normalized(
            it->body, it->in, 1, 10, result, error)) << error;
        double actual = 0.0;
        std::memcpy(&actual, result.raw.out.mem.data() + want.offset,
                    sizeof(actual));
        EXPECT_DOUBLE_EQ(actual, want.value) << want.name;
    }

    const auto status_it = std::find_if(
        catalog.begin(), catalog.end(), [](const auto& entry) {
            return entry.name == "x64_x87_fcompp_fnstsw";
        });
    ASSERT_NE(status_it, catalog.end());
    vitreo::oracle::perf::X64JitNormalizedResult status_result{};
    std::string error;
    ASSERT_TRUE(vitreo::oracle::perf::bench_x64_jit_normalized(
        status_it->body, status_it->in, 1, 10, status_result, error))
        << error;
    uint16_t status = 0;
    std::memcpy(&status, status_result.raw.out.mem.data() + 0x120u,
                sizeof(status));
    EXPECT_EQ(status, 0x0100u);  // 0.0 < 1.0: C0=1, C2=C3=0, TOP=0.
}

TEST(CpuPerfNormalizationTest, OptionalX64CatalogReport) {
    const char* output = std::getenv("VITREO_CPU_PERF_X64_JSON");
    if (output == nullptr || output[0] == '\0') {
        GTEST_SKIP() << "set VITREO_CPU_PERF_X64_JSON to write a report";
    }
    uint32_t trials = 5;
    if (const char* value = std::getenv("VITREO_CPU_PERF_X64_TRIALS")) {
        const unsigned long parsed = std::strtoul(value, nullptr, 10);
        if (parsed > 0 && parsed <= 100) {
            trials = static_cast<uint32_t>(parsed);
        }
    }
    const bool with_windows =
        std::getenv("VITREO_CPU_PERF_X64_WINDOWS") != nullptr;
    const auto report = vitreo::oracle::perf::bench_x64_catalog_trials(
        20, 2000, trials, with_windows);
    std::string error;
    ASSERT_TRUE(vitreo::oracle::perf::write_x64_report_json(report, output,
                                                            error))
        << error;
    EXPECT_FALSE(report.rows.empty());
    EXPECT_GT(report.comparable, 0u);
    if (with_windows) EXPECT_GT(report.windows_comparable, 0u);
}

TEST_F(CpuPerfOracleTest, X64StarterCatalogMeasuresOnNativeWindows) {
    std::string why;
    if (!windows64_ready(why)) {
        GTEST_SKIP() << "native Windows x64 oracle unavailable: " << why;
    }
    const auto report = vitreo::oracle::perf::bench_x64_catalog(20, 2000,
                                                                true);
    ASSERT_EQ(report.rows.size(),
              vitreo::oracle::perf::cpu_perf_x64_catalog().size());
    EXPECT_EQ(report.comparable, report.rows.size());
    EXPECT_EQ(report.windows_comparable, report.rows.size());
    for (const auto& row : report.rows) {
        ASSERT_TRUE(row.skip.empty()) << row.name << ": " << row.skip;
        ASSERT_TRUE(row.windows_skip.empty())
            << row.name << ": " << row.windows_skip;
        ASSERT_GT(row.operations_per_iter, 0u);
        if (row.multiblock) {
            EXPECT_EQ(row.jit.raw_stack_delta, 0) << row.name;
            EXPECT_EQ(row.jit.wrapper_stack_delta, 0) << row.name;
            EXPECT_EQ(row.windows.raw.gpr[4], row.windows.raw.initial_rsp)
                << row.name;
        }
        EXPECT_EQ(row.jit.raw.out.mem, row.windows.raw.mem) << row.name;
        EXPECT_EQ(row.jit.raw.out.mxcsr, row.windows.raw.mxcsr) << row.name;
        EXPECT_EQ(row.jit.raw.out.xmm, row.windows.raw.xmm) << row.name;
        EXPECT_EQ(row.jit.raw.out.rflags &
                      vitreo::oracle::contract::kArithFlagsMask,
                  row.windows.raw.rflags &
                      vitreo::oracle::contract::kArithFlagsMask)
            << row.name;
        if (row.name == "x64_add_dep" ||
            row.name == "x64_cmp_jcc_not_taken") {
            EXPECT_EQ(row.jit.raw.out.gpr[0], row.windows.raw.gpr[0])
                << row.name;
        } else if (row.name == "x64_high_gpr_mix") {
            EXPECT_EQ(row.jit.raw.out.gpr[8], row.windows.raw.gpr[8])
                << row.name;
        } else if (row.name == "x64_load64") {
            EXPECT_EQ(row.jit.raw.out.gpr[0], row.windows.raw.gpr[0])
                << row.name;
        }
        const double jit_net_op =
            row.jit.ns.net_ns_per_iter / row.operations_per_iter;
        const double windows_net_op =
            row.windows.ns.net_ns_per_iter / row.operations_per_iter;
        const double net_ratio = windows_net_op > 0.0
                                     ? jit_net_op / windows_net_op
                                     : 0.0;
        std::fprintf(stderr,
                     "[x64-catalog] %s jit_net=%.3fns/op "
                     "windows_net=%.3fns/op ratio=%.2fx\n",
                     row.name.c_str(), jit_net_op, windows_net_op, net_ratio);
    }
}
#endif

TEST_F(CpuPerfOracleTest, PrepareBodyStripsTrailingJcc) {
    // add eax, ecx; jnz +0
    const std::vector<uint8_t> bytes{0x01, 0xC8, 0x75, 0x00};
    std::vector<uint8_t> body;
    std::string why;
    ASSERT_TRUE(prepare_body(bytes, body, why)) << why;
    ASSERT_EQ(body.size(), 2u);
    EXPECT_EQ(body[0], 0x01);
    EXPECT_EQ(body[1], 0xC8);
}

TEST_F(CpuPerfOracleTest, PrepareBodyRejectsCall) {
    // call rel32
    const std::vector<uint8_t> bytes{0xE8, 0x00, 0x00, 0x00, 0x00};
    std::vector<uint8_t> body;
    std::string why;
    EXPECT_FALSE(prepare_body(bytes, body, why));
    EXPECT_FALSE(why.empty());
}

TEST_F(CpuPerfOracleTest, PrepareBodyNopsMidBlockJcc) {
    // add eax, ecx; jnz -2; nop — Jcc becomes NOPs so dump scratch
    // cannot take the branch out of the snippet (or spin).
    const std::vector<uint8_t> bytes{0x01, 0xC8, 0x75, 0xFE, 0x90};
    std::vector<uint8_t> body;
    std::string why;
    ASSERT_TRUE(prepare_body(bytes, body, why)) << why;
    ASSERT_EQ(body.size(), 5u);
    EXPECT_EQ(body[0], 0x01);
    EXPECT_EQ(body[1], 0xC8);
    EXPECT_EQ(body[2], 0x90);
    EXPECT_EQ(body[3], 0x90);
    EXPECT_EQ(body[4], 0x90);
}

TEST_F(CpuPerfOracleTest, DumpScratchAlignsEveryGpr) {
    const auto in = vitreo::oracle::perf::dump_scratch_state();
    for (int i = 0; i < 8; ++i) {
        EXPECT_GE(in.gpr[static_cast<size_t>(i)], vitreo::oracle::kMemVa)
            << "gpr " << i;
        EXPECT_LT(in.gpr[static_cast<size_t>(i)],
                  vitreo::oracle::kMemVa + 0x1000u)
            << "gpr " << i;
        EXPECT_EQ(in.gpr[static_cast<size_t>(i)] & 15u, 0u) << "gpr " << i;
    }
    EXPECT_NE(in.gpr[1], 16u);  // ECX must not stay the catalog REP count
}

TEST_F(CpuPerfOracleTest, WalkDumpBlockBenches) {
    // 0xb908f5 — hottest load block. TEST/JGE used to take the JIT
    // out of the snippet (status=5) before mid-block Jccs were NOPed.
    const std::vector<uint8_t> bytes{
        0x85, 0xed, 0x0f, 0x8d, 0x2f, 0x01, 0x00, 0x00, 0x8b, 0x54,
        0x24, 0x18, 0x8b, 0x0a, 0x3b, 0x4c, 0x24, 0x20, 0x0f, 0x82,
        0x1f, 0x01, 0x00, 0x00, 0x3b, 0x4c, 0x24, 0x1c, 0x0f, 0x83,
        0x15, 0x01, 0x00, 0x00, 0x83, 0x6c, 0x24, 0x14, 0x04, 0x8b,
        0xc1, 0xc1, 0xe8, 0x0c, 0x8b, 0xd0, 0x83, 0xe2, 0x07, 0x39,
        0x44, 0xd4, 0x28, 0x8d, 0x54, 0xd4, 0x28, 0x8b, 0xd9, 0x75,
        0x05, 0x8b, 0x7a, 0x04, 0xeb, 0x44,
    };
    std::vector<uint8_t> body;
    std::string why;
    ASSERT_TRUE(prepare_body(bytes, body, why)) << why;
    SnippetBench row;
    row.name = "0xb908f5";
    row.cls = "alu";
    bench_body(body, 10, 40, row, vitreo::oracle::perf::dump_scratch_state());
    ASSERT_TRUE(row.skip.empty()) << row.skip;
    EXPECT_GT(ratio(row), 0.0);
}

namespace {
const std::vector<uint8_t> kWalk0xb908f5{
    0x85, 0xed, 0x0f, 0x8d, 0x2f, 0x01, 0x00, 0x00, 0x8b, 0x54,
    0x24, 0x18, 0x8b, 0x0a, 0x3b, 0x4c, 0x24, 0x20, 0x0f, 0x82,
    0x1f, 0x01, 0x00, 0x00, 0x3b, 0x4c, 0x24, 0x1c, 0x0f, 0x83,
    0x15, 0x01, 0x00, 0x00, 0x83, 0x6c, 0x24, 0x14, 0x04, 0x8b,
    0xc1, 0xc1, 0xe8, 0x0c, 0x8b, 0xd0, 0x83, 0xe2, 0x07, 0x39,
    0x44, 0xd4, 0x28, 0x8d, 0x54, 0xd4, 0x28, 0x8b, 0xd9, 0x75,
    0x05, 0x8b, 0x7a, 0x04, 0xeb, 0x44,
};

void put_u32(vitreo::oracle::State& in, uint32_t va, uint32_t val) {
    const uint32_t off = va - vitreo::oracle::kMemVa;
    std::memcpy(in.mem.data() + off, &val, 4);
}

void put_xmm_f32(vitreo::oracle::State& in, size_t reg, size_t lane,
                 float val) {
    std::memcpy(in.xmm[reg].data() + lane * sizeof(val), &val, sizeof(val));
}
TEST_F(CpuPerfOracleTest, ClassCatalogThreeColumn) {
    // Per-instruction-class execution tax. Reporting test: it asserts only
    // that rows ran, because the whole point is to find the outliers.
    std::string why;
    const bool win = windows_ready(why);
    if (!win) {
        std::fprintf(stdout, "windows column unavailable: %s\n", why.c_str());
    }
    const Report report = bench_class_catalog(50, 400, win);
    print_report(report);
    for (const SnippetBench& row : report.rows) {
        if (!row.skip.empty()) {
            std::fprintf(stdout, "SKIP %-28s %s\n", row.name.c_str(),
                         row.skip.c_str());
        }
    }
    std::string write_error;
    if (const char* csv = std::getenv("VITREO_CPU_PERF_CSV")) {
        ASSERT_TRUE(write_report_csv(report, csv, write_error)) << write_error;
    }
    EXPECT_GT(report.comparable, 30u);
}

}  // namespace

TEST_F(CpuPerfOracleTest, WalkDumpLooped256) {
    // Amortize JIT/oracle enter-exit the same way walk_loop256 does.
    // ESI counter: the walk owns ECX.
    std::vector<uint8_t> body;
    std::string why;
    ASSERT_TRUE(prepare_body(kWalk0xb908f5, body, why)) << why;
    const auto looped = loop256_esi(body);
    ASSERT_FALSE(looped.empty());
    SnippetBench row;
    row.name = "0xb908f5_loop256";
    row.cls = "alu";
    bench_body(looped, 50, 200, row, vitreo::oracle::perf::dump_scratch_state());
    ASSERT_TRUE(row.skip.empty()) << row.skip;
    const double r = ratio(row);
    std::fprintf(stdout,
                 "0xb908f5 loop256 (Jcc NOP)  ros=%.1f jit=%.1f j/ros=%.2f\n",
                 static_cast<double>(row.oracle.run_ns) / row.oracle.iters,
                 static_cast<double>(row.jit.run_ns) / row.jit.iters, r);
    EXPECT_GT(r, 0.0);
    EXPECT_LT(r, 4.0);
}

TEST_F(CpuPerfOracleTest, WalkDumpLooped256WithJcc) {
    // Same loop, but keep the four not-taken Jccs and plant operands so
    // they fall through. This is the jit_cond_* tax the load sample saw.
    std::vector<uint8_t> body;
    std::string why;
    ASSERT_TRUE(prepare_body(kWalk0xb908f5, body, why, /*nop_jcc=*/false))
        << why;
    const auto looped = loop256_esi(body);
    ASSERT_FALSE(looped.empty());

    auto in = vitreo::oracle::perf::dump_scratch_state();
    in.gpr[5] = 0x80000000u;  // EBP < 0 → TEST; JGE falls through
    const uint32_t ecx_val = 0x04000090u;
    const uint32_t ecx_cell = 0x04000040u;
    const uint32_t esp = static_cast<uint32_t>(in.gpr[4]);
    put_u32(in, esp + 0x18u, ecx_cell);
    put_u32(in, ecx_cell, ecx_val);
    put_u32(in, esp + 0x20u, 0x04000000u);  // ECX >= → JB not taken
    put_u32(in, esp + 0x1cu, 0x04000100u);  // ECX <  → JAE not taken
    put_u32(in, esp + 0x28u, 0x4000u);      // EAX after SHR → JNZ not taken

    SnippetBench row;
    row.name = "0xb908f5_loop256_jcc";
    row.cls = "alu";
    bench_body(looped, 50, 200, row, in);
    ASSERT_TRUE(row.skip.empty()) << row.skip;
    const double r = ratio(row);
    std::fprintf(stdout,
                 "0xb908f5 loop256 (Jcc live) ros=%.1f jit=%.1f j/ros=%.2f\n",
                 static_cast<double>(row.oracle.run_ns) / row.oracle.iters,
                 static_cast<double>(row.jit.run_ns) / row.jit.iters, r);
    EXPECT_GT(r, 0.0);
    // Fused B.cond keeps this near the NOP-Jcc line; helpers used to land
    // around 4.2x, and catching a return to that is what this bound is for.
    //
    // The publishers in this loop all feed a Jcc that branches OUT of the
    // block, so their flags are live on the taken edge and the lazy-flag store
    // cannot simply be dropped -- doing that silently lost EFLAGS (see
    // OracleInteger32.TestEdiEdiJeSkipAdd). Keeping the store inline instead
    // cost 2.5x here. It now lives in the Jcc's cold exit stub, so the taken
    // edge stays correct and the hot fall-through body pays nothing:
    // measured 1.09-1.40x across 8 runs, against 1.09-1.26x before any of it.
    //
    // BOUND RAISED 2.0 -> 3.4 (2026-08-28). NOT a flag regression -- the flag
    // work above still holds. The default TSO configuration changed to
    // TsoMode::Fence with m_strict_tso, and `emit_tso_barrier` is a full
    // DMB ISH whose own comment says it "intentionally over-orders while
    // diagnosing x86 TSO publication/observation defects". The 32-bit guest
    // access path brackets EVERY linear load and store with two of them
    // (emit_guest_store32 and siblings, jit_compiler.cpp), and this snippet is
    // PE32. Measured on this exact test, same machine, same run:
    //
    //     default (Fence + strict)  2.50 - 2.80x
    //     VITREO_JIT_TSO_ORDERED=1  1.44x
    //     VITREO_JIT_NO_TSO=1       1.00x
    //
    // So ~2x of this row is the diagnostic barrier, not translation quality.
    // The x86-64 kernels barely move under the same knobs (load_chain 5.1 ->
    // 3.5x, everything else inside noise) because the x64 path uses the
    // LDAPR/STLR ordered forms instead. Revisit the default rather than this
    // bound; if the default becomes Ordered, put this back to 2.0.
    EXPECT_LT(r, 3.4);
}

TEST_F(CpuPerfOracleTest, NewtonRefineLoopedJitVsOracle) {
    // Reset the destructive XMM4 constant before the exact PE32 Newton-refine
    // body from OracleSseMore32.RsqrtpsNewtonRefine. XMM5 is invariant, so all
    // 256 iterations measure the same finite computation.
    const std::vector<uint8_t> kernel{
        0x0F, 0x28, 0xE5,        // movaps xmm4,xmm5
        0x0F, 0x28, 0xC1,        // movaps xmm0,xmm1
        0x0F, 0x52, 0xC0,        // rsqrtps xmm0,xmm0
        0x0F, 0x28, 0xD0,        // movaps xmm2,xmm0
        0x0F, 0x59, 0xD0,        // mulps xmm2,xmm0
        0x0F, 0x59, 0xD1,        // mulps xmm2,xmm1
        0x0F, 0x59, 0xD3,        // mulps xmm2,xmm3
        0x0F, 0x5C, 0xE2,        // subps xmm4,xmm2
        0x0F, 0x59, 0xC4,        // mulps xmm0,xmm4
    };
    const auto looped = loop256(kernel);
    ASSERT_FALSE(looped.empty());

    auto in = vitreo::oracle::perf::scratch_state();
    const float x[4] = {4.0f, 9.0f, 1.0f, 16.0f};
    for (size_t lane = 0; lane < 4; ++lane) {
        put_xmm_f32(in, 1, lane, x[lane]);
        put_xmm_f32(in, 3, lane, 0.5f);
        put_xmm_f32(in, 5, lane, 1.5f);
    }

    SnippetBench row;
    row.name = "newton_refine_loop256";
    row.cls = "sse";
    bench_body(looped, 50, 400, row, in);
    ASSERT_TRUE(row.skip.empty()) << row.skip;
    const double r = ratio(row);
    std::fprintf(stdout,
                 "newton refine loop256 ros=%.1f jit=%.1f j/ros=%.2f\n",
                 static_cast<double>(row.oracle.run_ns) / row.oracle.iters,
                 static_cast<double>(row.jit.run_ns) / row.jit.iters, r);
    EXPECT_GT(r, 0.0);
    EXPECT_LT(r, 4.0);
}

TEST_F(CpuPerfOracleTest, DumpScratchRunsAlignedMovaps) {
    // movaps xmm7, [ecx+0x30] — hung the dump replay when ECX was 16
    const std::vector<uint8_t> body{0x0F, 0x28, 0x79, 0x30};
    std::vector<uint8_t> prepared;
    std::string why;
    ASSERT_TRUE(prepare_body(body, prepared, why)) << why;
    SnippetBench row;
    row.name = "movaps_ecx30";
    row.cls = "sse";
    bench_body(prepared, 5, 20, row, vitreo::oracle::perf::dump_scratch_state());
    ASSERT_TRUE(row.skip.empty()) << row.skip;
    EXPECT_GT(ratio(row), 0.0);
}

TEST_F(CpuPerfOracleTest, AddRegJitNearOracle) {
    const std::vector<uint8_t> add_eax_ecx{0x01, 0xC8};
    SnippetBench row;
    row.name = "add_eax_ecx";
    row.cls = "alu";
    bench_body(add_eax_ecx, 50, 400, row);
    ASSERT_TRUE(row.skip.empty()) << row.skip;
    const double r = ratio(row);
    EXPECT_GT(r, 0.0);
    // Micro-op noise is large; this only proves both hosts ran.
    EXPECT_LT(r, 50.0);
}

TEST_F(CpuPerfOracleTest, Loop256MatchesHandWrittenWalk) {
    const std::vector<uint8_t> want{
        0xB9, 0x00, 0x01, 0x00, 0x00, 0x03, 0x06, 0x83, 0xC6, 0x04,
        0x83, 0xE9, 0x01, 0x75, 0xF6,
    };
    EXPECT_EQ(loop256({0x03, 0x06, 0x83, 0xC6, 0x04}), want);
}

TEST_F(CpuPerfOracleTest, WalkLoop256JitVsOracle) {
    const std::vector<uint8_t> walk_loop{
        0xB9, 0x00, 0x01, 0x00, 0x00,
        0x03, 0x06,
        0x83, 0xC6, 0x04,
        0x83, 0xE9, 0x01,
        0x75, 0xF6,
    };
    SnippetBench row;
    row.name = "walk_loop256";
    row.cls = "alu";
    bench_body(walk_loop, 50, 400, row);
    ASSERT_TRUE(row.skip.empty()) << row.skip;
    const double r = ratio(row);
    EXPECT_GT(r, 0.0);
    EXPECT_LT(r, 4.0);
}

TEST_F(CpuPerfOracleTest, WalkLikeLoadIsComparable) {
    // add eax, [esi]; add esi, 4  — package-walk shaped, no branch
    const std::vector<uint8_t> walk{0x03, 0x06, 0x83, 0xC6, 0x04};
    std::vector<uint8_t> body;
    std::string why;
    ASSERT_TRUE(prepare_body(walk, body, why)) << why;
    SnippetBench row;
    row.name = "walk_add";
    row.cls = "alu";
    bench_body(body, 50, 400, row);
    ASSERT_TRUE(row.skip.empty()) << row.skip;
    EXPECT_GT(ratio(row), 0.0);
}

TEST_F(CpuPerfOracleTest, ParseHotDumpAndBench) {
    const char* dir = std::getenv("TEST_TMPDIR");
    const std::string path = std::string(dir ? dir : "/tmp") +
                             "/cpu_perf_oracle_fixture.json";
    {
        std::ofstream out(path);
        ASSERT_TRUE(out);
        out << "[\n"
               "  {\"rip\":\"0x1000\",\"code_variant\":3,\"exec_count\":10,\"insn_count\":20,"
               "\"native_insns\":4,\"delegated\":false,\"class\":\"alu\","
               "\"accounting\":\"sampled_dispatcher\",\"sample_rate\":32,"
               "\"linked_hops_included\":false,"
               "\"capture_file\":\"hot-42.json\","
               "\"bytes\":\"01d07500\"},\n"
               "  {\"rip\":\"0x2000\",\"exec_count\":1,\"insn_count\":1,"
               "\"native_insns\":1,\"delegated\":true,\"class\":\"alu\","
               "\"bytes\":\"90\"}\n"
               "]\n";
    }
    std::vector<DumpBlock> blocks;
    std::string error;
    ASSERT_TRUE(parse_hot_dump(path, blocks, error)) << error;
    ASSERT_EQ(blocks.size(), 2u);
    EXPECT_EQ(blocks[0].rip, 0x1000u);
    EXPECT_EQ(blocks[0].code_variant, 3u);
    EXPECT_EQ(blocks[0].accounting, "sampled_dispatcher");
    EXPECT_EQ(blocks[0].sample_rate, 32u);
    EXPECT_FALSE(blocks[0].linked_hops_included);
    EXPECT_EQ(blocks[0].capture_file, "hot-42.json");
    EXPECT_TRUE(blocks[1].delegated);
    EXPECT_EQ(blocks[1].code_variant, UINT32_MAX);

    const Report report = bench_dump(blocks, 20, 80);
    EXPECT_EQ(report.comparable, 1u);
    EXPECT_EQ(report.skipped, 1u);
    EXPECT_GT(report.rows[0].replay_count, 1u);
    EXPECT_EQ(report.rows[0].capture_file, "hot-42.json");
    EXPECT_EQ(report.rows[0].code_variant, 3u);
    EXPECT_EQ(report.rows[0].name, "0x1000#3");
    std::string write_error;
    ASSERT_TRUE(write_report_json(report, path + ".report.json", write_error))
        << write_error;
    ASSERT_TRUE(write_report_csv(report, path + ".report.csv", write_error))
        << write_error;
    std::ifstream json(path + ".report.json");
    const std::string json_text((std::istreambuf_iterator<char>(json)),
                                std::istreambuf_iterator<char>());
    EXPECT_NE(json_text.find("\"schema\":\"vitreo.cpu_perf.v1\""),
              std::string::npos);
    EXPECT_NE(json_text.find("\"hotspot_gt_2x\":"), std::string::npos);
    EXPECT_NE(json_text.find("\"code_variant\":3"), std::string::npos);
    EXPECT_NE(json_text.find("\"code_variant\":null"), std::string::npos);
    print_report(report);
}

TEST_F(CpuPerfOracleTest, OptionalCapturedDump) {
    const char* path = std::getenv("VITREO_CPU_PERF_DUMP");
    if (path == nullptr || path[0] == '\0') {
        GTEST_SKIP() << "set VITREO_CPU_PERF_DUMP to a JIT hot-block JSON";
    }
    std::vector<DumpBlock> blocks;
    std::string error;
    ASSERT_TRUE(parse_hot_dump(path, blocks, error)) << error;
    const bool with_windows =
        std::getenv("VITREO_CPU_PERF_WINDOWS") != nullptr;
    const Report report = bench_dump(blocks, 20, 80, with_windows);
    print_report(report);
    std::string write_error;
    if (const char* json = std::getenv("VITREO_CPU_PERF_JSON")) {
        ASSERT_TRUE(write_report_json(report, json, write_error)) << write_error;
    }
    if (const char* csv = std::getenv("VITREO_CPU_PERF_CSV")) {
        ASSERT_TRUE(write_report_csv(report, csv, write_error)) << write_error;
    }
    EXPECT_FALSE(report.rows.empty());
}

TEST_F(CpuPerfOracleTest, CatalogLoopedJitVsOracle) {
    const auto catalog = cpu_perf_catalog();
    ASSERT_GE(catalog.size(), 10u);
    std::string why;
    const bool win = windows_ready(why);
    const Report report = bench_catalog(50, 400, win);
    print_report(report);
    ASSERT_EQ(report.skipped, 0u);
    ASSERT_EQ(report.comparable, static_cast<uint32_t>(catalog.size()));
    for (const SnippetBench& row : report.rows) {
        ASSERT_TRUE(row.skip.empty()) << row.name << ": " << row.skip;
        const double r = ratio(row);
        EXPECT_GT(r, 0.0) << row.name;
        // Linear 32-bit JIT is at or ahead of the host x86 runner on this catalog.
        EXPECT_LT(r, 4.0) << row.name << " JIT/oracle=" << r;
        if (win) {
            EXPECT_TRUE(row.windows_skip.empty())
                << row.name << ": " << row.windows_skip;
            EXPECT_GT(ratio_windows(row), 0.0) << row.name;
        }
    }
}

TEST_F(CpuPerfOracleTest, OptionalWindowsNative) {
    std::string why;
    if (!windows_ready(why)) {
        GTEST_SKIP() << why;
    }
    const Report report = bench_catalog(50, 400, /*with_windows=*/true);
    print_report(report);
    ASSERT_EQ(report.skipped, 0u);
    for (const SnippetBench& row : report.rows) {
        ASSERT_TRUE(row.skip.empty()) << row.name << ": " << row.skip;
        ASSERT_TRUE(row.windows_skip.empty())
            << row.name << ": " << row.windows_skip;
        EXPECT_GT(ratio(row), 0.0) << row.name;
        EXPECT_GT(ratio_windows(row), 0.0) << row.name;
        EXPECT_LT(ratio_windows(row), 50.0) << row.name;
    }
}

TEST(CpuPerfWindowsOracleTest, OptionalNativeX64ExecutesLongModeState) {
    std::string why;
    if (!windows64_ready(why)) {
        GTEST_SKIP() << why;
    }

    constexpr uint64_t kR8 = 0x1122334455667788ull;
    constexpr uint64_t kR9 = 0x8877665544332211ull;
    constexpr uint64_t kMemory = 0xFEDCBA9876543210ull;
    vitreo::oracle::State in{};
    in.mode = vitreo::oracle::kMode64;
    in.rflags = 0x202;
    in.gpr[4] = 0;  // Ask the runner for its controlled 64 KiB guest stack.
    in.gpr[8] = kR8;
    in.gpr[9] = kR9;
    in.gpr[10] = vitreo::oracle::kMemVa;
    std::memcpy(in.mem.data(), &kMemory, sizeof(kMemory));

    // The compatibility API still reports a semantic guest fault as false.
    vitreo::oracle::BenchTimes fault_times{};
    EXPECT_FALSE(bench_on_windows64({0x0F, 0x0B}, in, 1, 1, fault_times,
                                    why));
    EXPECT_EQ(fault_times.status, 1u);

    // Every failed predicate reaches UD2, which the runner reports as FAULT.
    // The local callee also verifies that CALL put its 64-bit return RIP at
    // [RSP] before RET consumes it.
    const std::vector<uint8_t> body = {
        0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        0x49, 0x39, 0xC0, 0x75, 0x28,
        0x48, 0xB8, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x49, 0x39, 0xC1, 0x75, 0x19,
        0x49, 0x8B, 0x02,
        0x49, 0xBB, 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE,
        0x4C, 0x39, 0xD8, 0x75, 0x07,
        0xE8, 0x04, 0x00, 0x00, 0x00,
        0xEB, 0x10,
        0x0F, 0x0B,
        0x4C, 0x8D, 0x1D, 0xF5, 0xFF, 0xFF, 0xFF,
        0x4C, 0x39, 0x1C, 0x24,
        0x75, 0xF1,
        0xC3,
    };

    vitreo::oracle::BenchTimes times{};
    ASSERT_TRUE(bench_on_windows64(body, in, 5, 100, times, why)) << why;
    EXPECT_EQ(times.status, 0u);
    EXPECT_GT(times.run_ns, 0u);
    EXPECT_GT(times.iters, 0u);
}

TEST(CpuPerfWindowsOracleTest, OptionalNativeX64ReportsStateAndFault) {
    std::string why;
    if (!windows64_ready(why)) {
        GTEST_SKIP() << why;
    }

    vitreo::oracle::State in{};
    in.mode = vitreo::oracle::kMode64;
    in.rflags = 0x202;
    in.gpr[4] = 0;
    in.gpr[10] = vitreo::oracle::kMemVa;
    in.mxcsr = 0x3F80;
    for (size_t i = 0; i < in.xmm[15].size(); ++i) {
        in.xmm[15][i] = static_cast<uint8_t>(0xA0u + i);
    }

    const std::vector<uint8_t> fault_body = {
        0x49, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        0x45, 0x31, 0xD2,
        0x49, 0x8B, 0x02,
    };
    vitreo::oracle::perf::Windows64BenchResult fault{};
    ASSERT_TRUE(bench_on_windows64_observed(fault_body, in, 1, 1, fault, why))
        << why;
    EXPECT_TRUE(why.empty());
    EXPECT_EQ(fault.times.status, 1u);
    EXPECT_EQ(fault.fault.code, 0xC0000005u);
    EXPECT_EQ(fault.fault.parameter_count, 2u);
    EXPECT_EQ(fault.fault.flags, 0u);
    EXPECT_EQ(fault.fault.address - fault.code_base, 13u);
    EXPECT_EQ(fault.rip, fault.fault.address);
    EXPECT_EQ(fault.fault.information[0], 0u);  // read access
    EXPECT_EQ(fault.fault.information[1], 0u);  // address zero
    EXPECT_EQ(fault.gpr[8], 0x1122334455667788ull);
    EXPECT_EQ(fault.gpr[10], 0u);
    EXPECT_EQ(fault.mxcsr, in.mxcsr);
    EXPECT_EQ(fault.xmm[15], in.xmm[15]);

    const std::vector<uint8_t> normal_body = {
        0x48, 0xB8, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0x49, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        0x48, 0xC7, 0xC1, 0xFF, 0xFF, 0xFF, 0xFF,
        0x48, 0x83, 0xC1, 0x01,
        0x4D, 0x89, 0x42, 0x08,
    };
    vitreo::oracle::perf::Windows64BenchResult normal{};
    ASSERT_TRUE(
        bench_on_windows64_observed(normal_body, in, 1, 10, normal, why))
        << why;
    EXPECT_EQ(normal.times.status, 0u);
    EXPECT_EQ(normal.fault.code, 0u);
    EXPECT_EQ(normal.gpr[0], 0x0102030405060708ull);
    EXPECT_EQ(normal.gpr[1], 0u);
    EXPECT_EQ(normal.gpr[8], 0x1122334455667788ull);
    EXPECT_EQ(normal.mxcsr, in.mxcsr);
    EXPECT_EQ(normal.xmm[15], in.xmm[15]);
    EXPECT_EQ(normal.gpr[4], normal.initial_rsp);
    EXPECT_EQ(normal.rip, normal.code_base + normal_body.size());
    EXPECT_EQ(normal.rflags & 0x8D5u, 0x55u);
    uint64_t stored = 0;
    std::memcpy(&stored, normal.mem.data() + 8, sizeof(stored));
    EXPECT_EQ(stored, 0x1122334455667788ull);

    vitreo::oracle::State bad = in;
    bad.gpr[4] = 1;
    vitreo::oracle::perf::Windows64BenchResult setup{};
    EXPECT_FALSE(bench_on_windows64_observed({0x90}, bad, 1, 1, setup, why));
    EXPECT_EQ(setup.times.status, 6u);
    EXPECT_FALSE(why.empty());

    bad = in;
    bad.mxcsr = 0x10000;
    EXPECT_FALSE(bench_on_windows64_observed({0x90}, bad, 1, 1, setup, why));
    EXPECT_EQ(setup.times.status, 6u);
}

TEST(CpuPerfWindowsOracleTest,
     OptionalNativeX64InstallsDeterministicEmptyX87State) {
    std::string why;
    if (!windows64_ready(why)) {
        GTEST_SKIP() << why;
    }

    vitreo::oracle::State in{};
    in.mode = vitreo::oracle::kMode64;
    in.gpr[4] = 0;
    in.fcw = 0x027F;
    in.fsw = 0;
    in.ftw = 0;

    vitreo::oracle::perf::Windows64BenchResult observed{};
    ASSERT_TRUE(bench_on_windows64_observed({}, in, 1, 1, observed, why))
        << why;
    ASSERT_EQ(observed.times.status, 0u);
    EXPECT_EQ(observed.fcw, 0x027Fu);
    EXPECT_EQ(observed.fsw, 0u);
    EXPECT_EQ(observed.ftw, 0u);

    // Also prove the full 80-bit ST payload is installed, not merely the
    // control/status words. 1.5L is significand C000... and exponent 3FFF.
    vitreo::oracle::State stacked = in;
    stacked.ftw = 0x01;
    stacked.st[0] = {0x00, 0x00, 0x00, 0x00, 0x00,
                     0x00, 0x00, 0xC0, 0xFF, 0x3F};
    stacked.gpr[11] = vitreo::oracle::kMemVa;
    const std::vector<uint8_t> pop_to_memory = {
        0x41, 0xDD, 0x9B, 0x30, 0x01, 0x00, 0x00};  // fstp [r11+0x130]
    ASSERT_TRUE(bench_on_windows64_observed(pop_to_memory, stacked, 1, 1,
                                            observed, why)) << why;
    ASSERT_EQ(observed.times.status, 0u);
    double value = 0.0;
    std::memcpy(&value, observed.mem.data() + 0x130, sizeof(value));
    EXPECT_DOUBLE_EQ(value, 1.5);
    EXPECT_EQ(observed.fcw, 0x027Fu);
    EXPECT_EQ(observed.fsw & 0x3800u, 0x0800u);  // POP advances TOP 0 -> 1.
    EXPECT_EQ(observed.ftw, 0u);
}

TEST(CpuPerfWindowsOracleTest,
     OptionalNativeX64ReportsX87ConstantStackOverflow) {
    std::string why;
    if (!windows64_ready(why)) {
        GTEST_SKIP() << why;
    }

    vitreo::oracle::State in{};
    in.mode = vitreo::oracle::kMode64;
    in.gpr[4] = 0;
    in.fcw = 0x027F;  // All x87 exceptions masked.
    in.fsw = 0;       // TOP=0.
    in.ftw = 0xFF;    // Every physical stack register is occupied.
    for (auto& st : in.st) {
        st = {0x00, 0x00, 0x00, 0x00, 0x00,
              0x00, 0x00, 0x80, 0xFF, 0x3F};  // +1.0L
    }

    constexpr uint16_t kDefinedStatus = 0xBAC1u;  // B, TOP, C1, ES, SF, IE.
    constexpr std::array<uint8_t, 10> kOne = {
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x80, 0xFF, 0x3F};
    constexpr std::array<uint8_t, 10> kIndefinite = {
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xC0, 0xFF, 0xFF};

    const auto run = [&](const std::vector<uint8_t>& body, uint16_t fcw) {
        vitreo::oracle::State state = in;
        state.fcw = fcw;
        vitreo::oracle::perf::Windows64BenchResult observed{};
        EXPECT_TRUE(bench_on_windows64_observed(body, state, 1, 1,
                                                observed, why)) << why;
        return observed;
    };

    // Intel SDM Vol. 1, "x87 FPU Stack Underflow or Overflow Exception":
    // masked overflow completes the push with the x87 indefinite value.
    // Vol. 2 FLD1/FLDZ defines C1=1 for stack overflow.  C0/C2/C3 are
    // undefined, so only architecturally meaningful status bits are checked.
    for (const uint8_t opcode : {uint8_t{0xE8}, uint8_t{0xEE}}) {
        SCOPED_TRACE(opcode == 0xE8 ? "FLD1" : "FLDZ");
        const auto masked = run({0xD9, opcode}, 0x027F);
        EXPECT_EQ(masked.times.status, 0u);
        EXPECT_EQ(masked.fcw, 0x027Fu);
        EXPECT_EQ(masked.fsw & kDefinedStatus, 0x3A41u);
        EXPECT_EQ(masked.ftw, 0xFFu);
        EXPECT_EQ(masked.st[0], kIndefinite);
        EXPECT_EQ(masked.fault.code, 0u);

        // With invalid-operation unmasked, FLD records the pending stack
        // fault but does not change TOP, tags, or ST(0). Delivery is deferred
        // until the explicit waiting instruction below.
        const auto pending = run({0xD9, opcode}, 0x027E);
        EXPECT_EQ(pending.times.status, 0u);
        EXPECT_EQ(pending.fcw, 0x027Eu);
        EXPECT_EQ(pending.fsw & kDefinedStatus, 0x82C1u);
        EXPECT_EQ(pending.ftw, 0xFFu);
        EXPECT_EQ(pending.st[0], kOne);
        EXPECT_EQ(pending.fault.code, 0u);

        const auto delivered = run({0xD9, opcode, 0x9B}, 0x027E);
        EXPECT_EQ(delivered.times.status, 1u);
        // Microsoft maps an x87 stack #MF to EXCEPTION_FLT_STACK_CHECK /
        // STATUS_FLOAT_STACK_CHECK, whose NTSTATUS value is 0xC0000092.
        EXPECT_EQ(delivered.fault.code, 0xC0000092u);
        EXPECT_EQ(delivered.fault.flags, 0u);
        EXPECT_EQ(delivered.fault.parameter_count, 1u);
        EXPECT_EQ(delivered.fault.information[0], 0u);
        EXPECT_EQ(delivered.fault.address, delivered.code_base + 2u);
        EXPECT_EQ(delivered.rip, delivered.fault.address);
        EXPECT_EQ(delivered.fcw, 0x027Eu);
        EXPECT_EQ(delivered.fsw & kDefinedStatus, 0x82C1u);
        EXPECT_EQ(delivered.ftw, 0xFFu);
        EXPECT_EQ(delivered.st[0], kOne);

        // Ordinary x87 arithmetic is itself waiting. It must deliver at its
        // own RIP before changing ST(0), exactly like explicit FWAIT.
        const auto arithmetic = run({0xD9, opcode, 0xD8, 0xC0}, 0x027E);
        EXPECT_EQ(arithmetic.times.status, 1u);
        EXPECT_EQ(arithmetic.fault.code, 0xC0000092u);
        EXPECT_EQ(arithmetic.fault.address, arithmetic.code_base + 2u);
        EXPECT_EQ(arithmetic.rip, arithmetic.fault.address);
        EXPECT_EQ(arithmetic.fsw & kDefinedStatus, 0x82C1u);
        EXPECT_EQ(arithmetic.ftw, 0xFFu);
        EXPECT_EQ(arithmetic.st[0], kOne);
    }

    // The FN clear/reset controls are intentionally non-waiting. They erase
    // the pending condition, so a following waiting FNOP executes normally.
    const auto cleared = run({0xD9, 0xEE, 0xDB, 0xE2, 0xD9, 0xD0}, 0x027E);
    EXPECT_EQ(cleared.times.status, 0u);
    EXPECT_EQ(cleared.fault.code, 0u);
    EXPECT_EQ(cleared.fcw, 0x027Eu);
    EXPECT_EQ(cleared.fsw & (0x80FFu), 0u); // B/ES/SF/sticky flags cleared.
    EXPECT_EQ(cleared.ftw, 0xFFu);

    const auto reset = run({0xD9, 0xEE, 0xDB, 0xE3, 0xD9, 0xD0}, 0x027E);
    EXPECT_EQ(reset.times.status, 0u);
    EXPECT_EQ(reset.fault.code, 0u);
    EXPECT_EQ(reset.fcw, 0x037Fu);
    EXPECT_EQ(reset.fsw, 0u);
    EXPECT_EQ(reset.ftw, 0u);

    // Seed a different pending class to prove the waiting rule is not
    // stack-check-specific and to pin Microsoft's #MF status mapping.
    vitreo::oracle::State divide = in;
    divide.fcw = 0x037B; // divide-by-zero unmasked
    divide.fsw = 0x0084; // ES | ZE
    vitreo::oracle::perf::Windows64BenchResult divide_observed{};
    ASSERT_TRUE(bench_on_windows64_observed({0xD9, 0xD0}, divide, 1, 1,
                                            divide_observed, why)) << why;
    EXPECT_EQ(divide_observed.times.status, 1u);
    EXPECT_EQ(divide_observed.fault.code, 0xC000008Eu);
    EXPECT_EQ(divide_observed.fault.address, divide_observed.code_base);
    EXPECT_EQ(divide_observed.rip, divide_observed.code_base);
    // Windows/Intel expose B together with the delivered pending condition.
    EXPECT_EQ(divide_observed.fsw & 0x8084u, 0x8084u);

    divide.fcw = 0x037F; // same recorded ZE, now masked
    ASSERT_TRUE(bench_on_windows64_observed({0xD9, 0xD0}, divide, 1, 1,
                                            divide_observed, why)) << why;
    EXPECT_EQ(divide_observed.times.status, 0u);
    EXPECT_EQ(divide_observed.fault.code, 0u);
}

TEST(CpuPerfWindowsOracleTest,
     OptionalNativeX64PblendwMasksMatchInterpreterAndJit) {
    std::string why;
    if (!windows64_ready(why)) {
        GTEST_SKIP() << why;
    }

    for (const uint8_t mask : {uint8_t{0x00}, uint8_t{0xFF}, uint8_t{0x55},
                               uint8_t{0xAA}, uint8_t{0xE7}}) {
        SCOPED_TRACE(::testing::Message() << "mask=0x" << std::hex
                                         << static_cast<unsigned>(mask));
        vitreo::oracle::State in{};
        in.mode = vitreo::oracle::kMode64;
        in.rflags = 0x202;
        in.gpr[4] = 0;
        in.mxcsr = 0x1F80;
        for (uint8_t i = 0; i < 8; ++i) {
            const uint16_t dst = static_cast<uint16_t>(0x1000u + i);
            const uint16_t src = static_cast<uint16_t>(0xA000u + i);
            std::memcpy(in.xmm[0].data() + i * 2, &dst, sizeof(dst));
            std::memcpy(in.xmm[1].data() + i * 2, &src, sizeof(src));
        }
        const std::vector<uint8_t> body = {
            0x66, 0x0F, 0x3A, 0x0E, 0xC1, mask};

        const vitreo::oracle::State interpreted =
            vitreo::oracle::contract::run_on_interpreter_64(in, body);
        const vitreo::oracle::State jitted =
            vitreo::oracle::contract::run_on_jit_64(in, body);
        ASSERT_EQ(interpreted.status, 0u);
        ASSERT_EQ(jitted.status, 0u);

        vitreo::oracle::perf::Windows64BenchResult windows{};
        ASSERT_TRUE(bench_on_windows64_observed(body, in, 1, 1, windows, why))
            << why;
        ASSERT_EQ(windows.times.status, 0u);
        EXPECT_EQ(interpreted.xmm[0], windows.xmm[0]);
        EXPECT_EQ(jitted.xmm[0], windows.xmm[0]);
        EXPECT_EQ(interpreted.rflags, windows.rflags);
        EXPECT_EQ(jitted.rflags, windows.rflags);
    }
}

TEST(CpuPerfWindowsOracleTest,
     OptionalNativeX64X87ArithmeticMatchesInterpreterAndJit) {
    std::string why;
    if (!windows64_ready(why)) {
        GTEST_SKIP() << why;
    }

    const auto catalog = vitreo::oracle::perf::cpu_perf_x64_catalog();
    struct Case {
        const char* name;
        size_t offset;
        double expected;
    };
    const Case cases[] = {
        {"x64_x87_fld1_fadd_fstp", 0x100u, 2.0},
        {"x64_x87_fld_fadd_m64", 0x108u, 3.75},
        {"x64_x87_fld_fmul_m64", 0x110u, 3.375},
    };
    for (const Case& test : cases) {
        SCOPED_TRACE(test.name);
        const auto entry = std::find_if(
            catalog.begin(), catalog.end(), [&](const auto& candidate) {
                return candidate.name == test.name;
            });
        ASSERT_NE(entry, catalog.end());

        const vitreo::oracle::State interpreted =
            vitreo::oracle::contract::run_on_interpreter_64(entry->in,
                                                            entry->body);
        const vitreo::oracle::State jitted =
            vitreo::oracle::contract::run_on_jit_64(entry->in, entry->body);
        ASSERT_EQ(interpreted.status, 0u);
        ASSERT_EQ(jitted.status, 0u);

        vitreo::oracle::perf::Windows64BenchResult windows{};
        ASSERT_TRUE(bench_on_windows64_observed(entry->body, entry->in, 1, 1,
                                                windows, why)) << why;
        ASSERT_EQ(windows.times.status, 0u);
        double interpreter_value = 0.0;
        double jit_value = 0.0;
        double windows_value = 0.0;
        std::memcpy(&interpreter_value,
                    interpreted.mem.data() + test.offset,
                    sizeof(interpreter_value));
        std::memcpy(&jit_value, jitted.mem.data() + test.offset,
                    sizeof(jit_value));
        std::memcpy(&windows_value, windows.mem.data() + test.offset,
                    sizeof(windows_value));
        EXPECT_DOUBLE_EQ(interpreter_value, test.expected);
        EXPECT_DOUBLE_EQ(jit_value, interpreter_value);
        EXPECT_DOUBLE_EQ(windows_value, interpreter_value);
        EXPECT_EQ(jitted.fcw, interpreted.fcw);
        EXPECT_EQ(jitted.fsw, interpreted.fsw);
        EXPECT_EQ(jitted.ftw, interpreted.ftw);
        EXPECT_EQ(windows.fcw, interpreted.fcw);
        EXPECT_EQ(windows.fsw, interpreted.fsw);
        EXPECT_EQ(windows.ftw, interpreted.ftw);
        EXPECT_EQ(jitted.rflags, interpreted.rflags);
        EXPECT_EQ(windows.rflags & 0x8D5u, interpreted.rflags & 0x8D5u);
    }
}

TEST(CpuPerfWindowsOracleTest,
     OptionalNativeX64FcomppClearsC1AndPreservesIntegerFlags) {
    std::string why;
    if (!windows64_ready(why)) {
        GTEST_SKIP() << why;
    }

    vitreo::oracle::State in{};
    in.mode = vitreo::oracle::kMode64;
    in.gpr[1] = 7;  // RCX
    in.gpr[3] = 0x1122334455667700ull;  // RBX, BL receives SETZ
    in.gpr[4] = 0;
    in.fcw = 0x027F;
    in.fsw = 0x0200u;  // Seed C1=1; valid FCOMPP must clear it.
    in.ftw = 0;
    const std::vector<uint8_t> body = {
        0x39, 0xC9,              // cmp ecx,ecx (ZF=1)
        0xD9, 0xE8,              // fld1
        0xD9, 0xEE,              // fldz
        0xDE, 0xD9,              // fcompp: 0 < 1
        0xDF, 0xE0,              // fnstsw ax
        0x0F, 0x94, 0xC3,        // setz bl (x87 must preserve ZF)
    };

    const vitreo::oracle::State interpreted =
        vitreo::oracle::contract::run_on_interpreter_64(in, body);
    const vitreo::oracle::State jitted =
        vitreo::oracle::contract::run_on_jit_64(in, body);
    ASSERT_EQ(interpreted.status, 0u);
    ASSERT_EQ(jitted.status, 0u);
    vitreo::oracle::perf::Windows64BenchResult windows{};
    ASSERT_TRUE(bench_on_windows64_observed(body, in, 1, 1, windows, why))
        << why;
    ASSERT_EQ(windows.times.status, 0u);

    EXPECT_EQ(interpreted.fsw, 0x0100u);
    EXPECT_EQ(jitted.fsw, interpreted.fsw);
    EXPECT_EQ(windows.fsw, interpreted.fsw);
    EXPECT_EQ(interpreted.ftw, 0u);
    EXPECT_EQ(jitted.ftw, interpreted.ftw);
    EXPECT_EQ(windows.ftw, interpreted.ftw);
    EXPECT_EQ(interpreted.gpr[3] & 0xFFu, 1u);
    EXPECT_EQ(jitted.gpr[3] & 0xFFu, 1u);
    EXPECT_EQ(windows.gpr[3] & 0xFFu, 1u);
    EXPECT_NE(interpreted.rflags & 0x40u, 0u);
    EXPECT_EQ(jitted.rflags & 0x8D5u, interpreted.rflags & 0x8D5u);
    EXPECT_EQ(windows.rflags & 0x8D5u, interpreted.rflags & 0x8D5u);
}

TEST(CpuPerfWindowsOracleTest,
     OptionalNativeX64DefersUnmaskedX87ProducerFaultsAtomically) {
    std::string why;
    if (!windows64_ready(why)) {
        GTEST_SKIP() << why;
    }

    constexpr std::array<uint8_t, 10> kZero{};
    constexpr std::array<uint8_t, 10> kHalf = {
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x80, 0xFE, 0x3F};
    constexpr std::array<uint8_t, 10> kOne = {
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x80, 0xFF, 0x3F};
    constexpr std::array<uint8_t, 10> kOnePointFive = {
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xC0, 0xFF, 0x3F};

    const auto probe = [&](const char* name, const std::vector<uint8_t>& body,
                           const vitreo::oracle::State& in) {
        vitreo::oracle::perf::Windows64BenchResult out{};
        EXPECT_TRUE(bench_on_windows64_observed(body, in, 1, 1, out, why))
            << name << ": " << why;
        return out;
    };

    vitreo::oracle::State underflow{};
    underflow.mode = vitreo::oracle::kMode64;
    underflow.gpr[1] = 5;
    underflow.gpr[2] = 5;
    underflow.fcw = 0x027E;  // invalid operation unmasked
    // AMD APM Vol. 1 section 6.8.2: an unmasked x87 producer suspends the
    // faulting operation and #MF is raised at the next waiting x87 boundary.
    // Microsoft GetExceptionCode maps stack #MF to STATUS_FLOAT_STACK_CHECK.
    const auto underflow_pending =
        probe("underflow", {0xD8, 0xC0}, underflow);
    EXPECT_EQ(underflow_pending.times.status, 0u);
    EXPECT_EQ(underflow_pending.fault.code, 0u);
    EXPECT_EQ(underflow_pending.fsw, 0x80C1u);  // B|ES|SF|IE, C1=0.
    EXPECT_EQ(underflow_pending.ftw, 0u);
    EXPECT_EQ(underflow_pending.st, underflow.st);

    const auto underflow_delivered = probe(
        "underflow_wait", {0x39, 0xD1, 0xD8, 0xC0, 0x9B}, underflow);
    EXPECT_EQ(underflow_delivered.times.status, 1u);
    EXPECT_EQ(underflow_delivered.fault.code, 0xC0000092u);
    EXPECT_EQ(underflow_delivered.fault.address,
              underflow_delivered.code_base + 4u);
    EXPECT_EQ(underflow_delivered.rip, underflow_delivered.fault.address);
    EXPECT_EQ(underflow_delivered.fsw, 0x80C1u);
    EXPECT_EQ(underflow_delivered.ftw, 0u);
    EXPECT_EQ(underflow_delivered.st, underflow.st);
    // The exception context adds RF, but x87 delivery must retain CMP's
    // integer arithmetic flags (equal operands set PF and ZF here).
    EXPECT_EQ(underflow_delivered.rflags & 0x8D5u, 0x44u);
    EXPECT_NE(underflow_delivered.rflags & 0x10000u, 0u);

    vitreo::oracle::State divide = underflow;
    divide.fcw = 0x027B;  // divide-by-zero unmasked
    divide.ftw = 0x03;
    divide.st[0] = kOne;
    divide.st[1] = kZero;
    const auto divide_pending = probe("divide", {0xD8, 0xF1}, divide);
    EXPECT_EQ(divide_pending.times.status, 0u);
    EXPECT_EQ(divide_pending.fault.code, 0u);
    EXPECT_EQ(divide_pending.fsw, 0x8084u);  // B|ES|ZE.
    EXPECT_EQ(divide_pending.ftw, 0x03u);
    EXPECT_EQ(divide_pending.st, divide.st);

    const auto divide_delivered =
        probe("divide_wait", {0x39, 0xD1, 0xD8, 0xF1, 0x9B}, divide);
    EXPECT_EQ(divide_delivered.times.status, 1u);
    EXPECT_EQ(divide_delivered.fault.code, 0xC000008Eu);
    EXPECT_EQ(divide_delivered.fault.address,
              divide_delivered.code_base + 4u);
    EXPECT_EQ(divide_delivered.rip, divide_delivered.fault.address);
    EXPECT_EQ(divide_delivered.fsw, 0x8084u);
    EXPECT_EQ(divide_delivered.ftw, 0x03u);
    EXPECT_EQ(divide_delivered.st, divide.st);
    EXPECT_EQ(divide_delivered.rflags & 0x8D5u, 0x44u);
    EXPECT_NE(divide_delivered.rflags & 0x10000u, 0u);

    vitreo::oracle::State full = underflow;
    full.ftw = 0xFF;
    full.st.fill(kOne);
    for (const auto& [name, opcode, st0] : {
             std::tuple<const char*, uint8_t, std::array<uint8_t, 10>>{
                 "fsincos", 0xFB, kHalf},
             {"fptan", 0xF2, kHalf},
             {"fxtract", 0xF4, kOnePointFive}}) {
        full.st[0] = st0;
        const auto pending = probe(name, {0xD9, opcode}, full);
        EXPECT_EQ(pending.times.status, 0u);
        EXPECT_EQ(pending.fault.code, 0u);
        EXPECT_EQ(pending.fsw, 0x82C1u);  // B|ES|SF|C1|IE, TOP=0.
        EXPECT_EQ(pending.ftw, 0xFFu);
        EXPECT_EQ(pending.st, full.st);

        const auto delivered = probe(
            (std::string(name) + "_wait").c_str(),
            {0x39, 0xD1, 0xD9, opcode, 0x9B}, full);
        EXPECT_EQ(delivered.times.status, 1u);
        EXPECT_EQ(delivered.fault.code, 0xC0000092u);
        EXPECT_EQ(delivered.fault.address, delivered.code_base + 4u);
        EXPECT_EQ(delivered.rip, delivered.fault.address);
        EXPECT_EQ(delivered.fsw, 0x82C1u);
        EXPECT_EQ(delivered.ftw, 0xFFu);
        EXPECT_EQ(delivered.st, full.st);
        EXPECT_EQ(delivered.rflags & 0x8D5u, 0x44u);
        EXPECT_NE(delivered.rflags & 0x10000u, 0u);
    }
}

TEST(CpuPerfWindowsOracleTest,
     OptionalNativeX64UnmaskedOverflowUnderflowExposeScaledRegisterResults) {
    std::string why;
    if (!windows64_ready(why)) {
        GTEST_SKIP() << why;
    }

    constexpr std::array<uint8_t, 10> kLargest = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFE, 0x7F};
    constexpr std::array<uint8_t, 10> kSmallestNormal = {
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x80, 0x01, 0x00};
    constexpr std::array<uint8_t, 10> kTwo = {
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x80, 0x00, 0x40};
    constexpr std::array<uint8_t, 10> kThree = {
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xC0, 0x00, 0x40};
    constexpr std::array<uint8_t, 10> kScaledOverflow = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0x1F};
    constexpr std::array<uint8_t, 10> kScaledUnderflow = {
        0xAB, 0xAA, 0xAA, 0xAA, 0xAA,
        0xAA, 0xAA, 0xAA, 0xFF, 0x5F};

    const auto exercise = [&](const char* name, vitreo::oracle::State in,
                              const std::vector<uint8_t>& producer,
                              const std::array<uint8_t, 10>& expected_st0,
                              uint16_t expected_fsw,
                              uint32_t expected_fault) {
        const vitreo::oracle::State interpreted =
            vitreo::oracle::contract::run_on_interpreter_64(in, producer);
        const vitreo::oracle::State jitted =
            vitreo::oracle::contract::run_on_jit_64(in, producer);
        ASSERT_EQ(interpreted.status, 0u) << name;
        ASSERT_EQ(jitted.status, 0u) << name;

        vitreo::oracle::perf::Windows64BenchResult windows{};
        ASSERT_TRUE(bench_on_windows64_observed(
            producer, in, 1, 1, windows, why)) << name << ": " << why;
        ASSERT_EQ(windows.times.status, 0u) << name;
        EXPECT_EQ(interpreted.fsw, expected_fsw) << name;
        EXPECT_EQ(jitted.fsw, expected_fsw) << name;
        EXPECT_EQ(windows.fsw, expected_fsw) << name;
        EXPECT_EQ(interpreted.st[0], expected_st0) << name;
        EXPECT_EQ(jitted.st[0], expected_st0) << name;
        EXPECT_EQ(windows.st[0], expected_st0) << name;
        EXPECT_EQ(interpreted.ftw, 0x03u) << name;
        EXPECT_EQ(jitted.ftw, 0x03u) << name;
        EXPECT_EQ(windows.ftw, 0x03u) << name;

        std::vector<uint8_t> waiting = producer;
        waiting.push_back(0x9B); // FWAIT delivers the already-committed state
        vitreo::oracle::perf::Windows64BenchResult delivered{};
        ASSERT_TRUE(bench_on_windows64_observed(
            waiting, in, 1, 1, delivered, why)) << name << ": " << why;
        EXPECT_EQ(delivered.times.status, 1u) << name;
        EXPECT_EQ(delivered.fault.code, expected_fault) << name;
        EXPECT_EQ(delivered.fault.address,
                  delivered.code_base + producer.size()) << name;
        EXPECT_EQ(delivered.rip, delivered.fault.address) << name;
        EXPECT_EQ(delivered.fsw, expected_fsw) << name;
        EXPECT_EQ(delivered.st[0], expected_st0) << name;
    };

    vitreo::oracle::State overflow{};
    overflow.mode = vitreo::oracle::kMode64;
    overflow.fcw = 0x0377; // PC=64, nearest, #O unmasked
    overflow.ftw = 0x03;
    overflow.st[0] = kLargest;
    overflow.st[1] = kTwo;
    exercise("overflow", overflow, {0xD8, 0xC9}, kScaledOverflow,
             0x8088u, 0xC0000091u);

    vitreo::oracle::State underflow{};
    underflow.mode = vitreo::oracle::kMode64;
    underflow.fcw = 0x036F; // PC=64, nearest, #U unmasked
    underflow.ftw = 0x03;
    underflow.st[0] = kSmallestNormal;
    underflow.st[1] = kThree;
    exercise("underflow", underflow, {0xD8, 0xF1}, kScaledUnderflow,
             0x82B0u, 0xC0000093u);
}

TEST(CpuPerfWindowsOracleTest,
     OptionalNativeX64ReportsDenormalOperandAndDefersExactFault) {
    std::string why;
    if (!windows64_ready(why)) {
        GTEST_SKIP() << why;
    }

    constexpr std::array<uint8_t, 10> kDenormal = {
        0x01, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00};
    constexpr std::array<uint8_t, 10> kTwiceDenormal = {
        0x02, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00};

    vitreo::oracle::State in{};
    in.mode = vitreo::oracle::kMode64;
    in.gpr[1] = 7;
    in.gpr[2] = 7;
    in.ftw = 0x01;
    in.st[0] = kDenormal;

    vitreo::oracle::perf::Windows64BenchResult masked{};
    in.fcw = 0x037F;
    ASSERT_TRUE(bench_on_windows64_observed(
        {0xD8, 0xC0}, in, 1, 1, masked, why)) << why; // FADD ST0,ST0
    ASSERT_EQ(masked.times.status, 0u);
    EXPECT_EQ(masked.fault.code, 0u);
    EXPECT_EQ(masked.fsw, 0x0002u); // DE
    EXPECT_EQ(masked.ftw, 0x01u);
    EXPECT_EQ(masked.st[0], kTwiceDenormal);

    // AMD APM Vol. 1 section 6.8.2.3 also requires #D when FLD widens a
    // denormal binary32/binary64 memory source. Pin binary32 separately: its
    // encoding becomes a normal extended value, so checking ST after the
    // widening is too late to recover the source exception class.
    vitreo::oracle::State load32{};
    load32.mode = vitreo::oracle::kMode64;
    load32.gpr[11] = vitreo::oracle::kMemVa;
    load32.fcw = 0x037F;
    const uint32_t min_binary32_subnormal = 1;
    std::memcpy(load32.mem.data(), &min_binary32_subnormal,
                sizeof(min_binary32_subnormal));
    vitreo::oracle::perf::Windows64BenchResult loaded32{};
    ASSERT_TRUE(bench_on_windows64_observed(
        {0x41, 0xD9, 0x03}, load32, 1, 1, loaded32, why)) << why; // FLD [r11]
    ASSERT_EQ(loaded32.times.status, 0u);
    EXPECT_EQ(loaded32.fault.code, 0u);
    EXPECT_EQ(loaded32.fsw & 0x003Fu, 0x0002u); // DE, not UE

    vitreo::oracle::perf::Windows64BenchResult unmasked{};
    in.fcw = 0x037D;
    ASSERT_TRUE(bench_on_windows64_observed(
        {0x39, 0xD1, 0xD8, 0xC0, 0x9B}, in, 1, 1, unmasked, why)) << why;
    ASSERT_EQ(unmasked.times.status, 1u);
    // Windows reports deferred x87 #D as INVALID_OPERATION even though FSW
    // precisely identifies DE. This is measured VEH behavior, not inferred
    // from the existence of STATUS_FLOAT_DENORMAL_OPERAND.
    EXPECT_EQ(unmasked.fault.code, 0xC0000090u);
    EXPECT_EQ(unmasked.fault.address, unmasked.code_base + 4u);
    EXPECT_EQ(unmasked.rip, unmasked.fault.address);
    EXPECT_EQ(unmasked.fsw, 0x8082u); // B | ES | DE
    EXPECT_EQ(unmasked.ftw, 0x01u);
    EXPECT_EQ(unmasked.st[0], kDenormal); // producer commit suppressed
    EXPECT_EQ(unmasked.rflags & 0x8D5u, 0x44u); // CMP flags survive #MF
    EXPECT_NE(unmasked.rflags & 0x10000u, 0u);  // Windows sets RF in context
}

TEST(CpuPerfWindowsOracleTest,
     OptionalNativeX64FldcwUnmaskDefersStickyExceptionToNextWait) {
    std::string why;
    if (!windows64_ready(why)) {
        GTEST_SKIP() << why;
    }

    vitreo::oracle::State in{};
    in.mode = vitreo::oracle::kMode64;
    in.gpr[1] = 5;
    in.gpr[2] = 5;
    in.gpr[11] = vitreo::oracle::kMemVa;
    in.fcw = 0x037F; // divide-by-zero masked
    in.fsw = 0x0004; // ZE sticky, but ES/B clear
    const uint16_t unmasked = 0x037B;
    std::memcpy(in.mem.data() + 0x130, &unmasked, sizeof(unmasked));

    // cmp ecx,edx; fldcw [r11+0x130]; fnstsw ax; fwait
    const std::vector<uint8_t> body = {
        0x39, 0xD1,
        0x41, 0xD9, 0xAB, 0x30, 0x01, 0x00, 0x00,
        0xDF, 0xE0,
        0x9B,
    };
    vitreo::oracle::perf::Windows64BenchResult observed{};
    ASSERT_TRUE(bench_on_windows64_observed(body, in, 1, 1, observed, why))
        << why;
    EXPECT_EQ(observed.times.status, 1u);
    EXPECT_EQ(observed.fault.code, 0xC000008Eu);
    EXPECT_EQ(observed.fault.address, observed.code_base + 11u);
    EXPECT_EQ(observed.rip, observed.fault.address);
    EXPECT_EQ(observed.fcw, 0x037Bu);
    EXPECT_EQ(observed.fsw, 0x8084u);
    EXPECT_EQ(observed.gpr[0] & 0xFFFFu, 0x8084u);
    EXPECT_EQ(observed.rflags & 0x8D5u, 0x44u);
    EXPECT_NE(observed.rflags & 0x10000u, 0u);

}

TEST(CpuPerfWindowsOracleTest, OptionalNativeX64NormalizesWrapperCost) {
    std::string why;
    if (!windows64_ready(why)) {
        GTEST_SKIP() << why;
    }

    constexpr uint64_t kR8 = 0x1122334455667788ull;
    constexpr uint64_t kR9 = 0x8877665544332211ull;
    constexpr uint64_t kMem = 0xFEDCBA9876543210ull;
    vitreo::oracle::State in{};
    in.mode = vitreo::oracle::kMode64;
    in.rflags = 0x202;
    in.gpr[4] = 0;
    in.gpr[8] = kR8;
    in.gpr[9] = kR9;
    std::memcpy(in.mem.data(), &kMem, sizeof(kMem));

    vitreo::oracle::perf::Windows64BenchResult empty{};
    ASSERT_TRUE(bench_on_windows64_observed({}, in, 20, 2000, empty, why))
        << why;
    EXPECT_EQ(empty.times.status, 0u);
    EXPECT_GT(empty.times.run_ns, 0u);
    EXPECT_GT(empty.times.iters, 0u);
    EXPECT_EQ(empty.rip, empty.code_base);
    EXPECT_EQ(empty.gpr[4], empty.initial_rsp);
    EXPECT_EQ(empty.gpr[8], kR8);
    EXPECT_EQ(empty.gpr[9], kR9);
    EXPECT_EQ(empty.fault.code, 0u);
    uint64_t returned_mem = 0;
    std::memcpy(&returned_mem, empty.mem.data(), sizeof(returned_mem));
    EXPECT_EQ(returned_mem, kMem);

    const std::vector<uint8_t> loop = {
        0xB9, 0x00, 0x01, 0x00, 0x00,  // mov ecx, 256
        0x4C, 0x01, 0xC0,              // add rax, r8
        0xFF, 0xC9,                    // dec ecx
        0x75, 0xF9,                    // jnz add
    };
    vitreo::oracle::State loop_in = in;
    loop_in.gpr[0] = 0;
    loop_in.gpr[8] = 1;
    vitreo::oracle::perf::Windows64NormalizedBenchResult normalized{};
    std::array<double, 5> raw_trials{};
    std::array<double, 5> wrapper_trials{};
    std::array<double, 5> net_trials{};
    for (size_t i = 0; i < net_trials.size(); ++i) {
        ASSERT_TRUE(bench_on_windows64_normalized(loop, loop_in, 20, 2000,
                                                  normalized, why))
            << why;
        raw_trials[i] = normalized.ns.raw_ns_per_iter;
        wrapper_trials[i] = normalized.ns.wrapper_ns_per_iter;
        net_trials[i] = normalized.ns.net_ns_per_iter;
    }
    std::sort(raw_trials.begin(), raw_trials.end());
    std::sort(wrapper_trials.begin(), wrapper_trials.end());
    std::sort(net_trials.begin(), net_trials.end());
    constexpr size_t kMedian = 2;
    std::printf("x64 windows loop256 median raw=%.3f wrapper=%.3f net=%.3f "
                "ns/iter\n",
                raw_trials[kMedian], wrapper_trials[kMedian],
                net_trials[kMedian]);
    EXPECT_EQ(normalized.raw.times.iters, normalized.wrapper.iters);
    EXPECT_EQ(normalized.raw.gpr[0], 256u);
    EXPECT_EQ(normalized.raw.gpr[1], 0u);
    EXPECT_EQ(normalized.raw.rip,
              normalized.raw.code_base + loop.size());
    EXPECT_TRUE(std::isfinite(normalized.ns.raw_ns_per_iter));
    EXPECT_TRUE(std::isfinite(normalized.ns.wrapper_ns_per_iter));
    EXPECT_TRUE(std::isfinite(normalized.ns.net_ns_per_iter));
    EXPECT_GT(net_trials[kMedian], 0.0);
}

TEST(CpuPerfWindowsOracleTest,
     OptionalNativeX64RejectsUnsupportedLowMxcsrBits) {
    std::string why;
    if (!windows64_ready(why)) {
        GTEST_SKIP() << why;
    }

    vitreo::oracle::State in{};
    in.mode = vitreo::oracle::kMode64;
    in.gpr[4] = 0;
    for (uint32_t bit = 0; bit < 16; ++bit) {
        const uint32_t candidate = in.mxcsr | (1u << bit);
        if (candidate == in.mxcsr) continue;
        vitreo::oracle::State probe = in;
        probe.mxcsr = candidate;
        vitreo::oracle::perf::Windows64BenchResult result{};
        if (!bench_on_windows64_observed({0x90}, probe, 1, 1, result, why)) {
            EXPECT_EQ(result.times.status, 6u) << "MXCSR bit " << bit;
            return;
        }
    }
    GTEST_SKIP() << "Windows host FXSAVE MXCSR_MASK advertises every low bit";
}

}  // namespace
