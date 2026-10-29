// SPDX-License-Identifier: LGPL-2.1-only
#include "common/divergence.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace vitreo::fuzz {
namespace {

TEST(FuzzDivergence, BisectionFindsEarliestMonotonicFailure) {
    std::vector<size_t> probes;
    const auto result = bisect_first_failing_prefix(
        16, [&](size_t index) {
            probes.push_back(index);
            return index >= 6;
        });

    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.prefix_index, 6u);
    EXPECT_LT(result.probes, 16u);
}

TEST(FuzzDivergence, BisectionVerifiesNonMonotonicEarlierFailure) {
    const auto result = bisect_first_failing_prefix(
        8, [](size_t index) {
            return index == 1 || index >= 6;
        });

    ASSERT_TRUE(result.found);
    EXPECT_EQ(result.prefix_index, 1u);
}

TEST(FuzzDivergence, DecodesOnlyWholeInstructionPrefixes) {
    FuzzState input;
    input.arch = Arch::X86;
    input.code_bytes = {0x01, 0xc8, 0x66, 0x31, 0xd2};

    std::string error;
    EXPECT_EQ(decode_prefix_lengths(input, &error),
              (std::vector<size_t>{2u, 5u}));
    EXPECT_TRUE(error.empty());
}

TEST(FuzzDivergence, RejectsTruncatedInstructionStream) {
    FuzzState input;
    input.arch = Arch::X86;
    input.code_bytes = {0x0f};

    std::string error;
    EXPECT_TRUE(decode_prefix_lengths(input, &error).empty());
    EXPECT_NE(error.find("byte 0"), std::string::npos);
}

TEST(FuzzDivergence, HotBlocksConvertOnlySelfContainedCode) {
    const auto conversions = parse_hot_blocks(
        R"([
          {"rip":"0x1000","bytes":"01c8"},
          {"rip":"0x2000","bytes":"8b00"},
          {"rip":"0x3000","bytes":"ebfe"}
        ])");

    ASSERT_EQ(conversions.size(), 3u);
    EXPECT_TRUE(conversions[0].supported);
    EXPECT_FALSE(conversions[1].supported);
    EXPECT_FALSE(conversions[2].supported);
    EXPECT_EQ(conversions[0].input.kind, TestKind::BasicBlock);
}

TEST(FuzzDivergence, ReportIsMachineReadableAndNamesBothRunners) {
    DivergenceReport report;
    report.source = "sample.json";
    report.status = DivergenceStatus::Diverged;
    report.input.arch = Arch::X86;
    report.input.kind = TestKind::BasicBlock;
    report.input.code_bytes = {0x01, 0xc8};
    report.instruction_count = 1;
    report.first_divergent_instruction = 1;
    report.prefix_bytes = 2;
    report.difference.equal = false;
    report.difference.items.push_back(
        {"GPR.EAX", "0x00000002", "0x00000001"});

    const std::string json = serialize_divergence_report(report);
    EXPECT_NE(json.find("\"status\":\"diverged\""), std::string::npos);
    EXPECT_NE(json.find("\"first_divergent_instruction\":1"),
              std::string::npos);
    EXPECT_NE(json.find("\"jit\":\"0x00000002\""), std::string::npos);
    EXPECT_NE(json.find("\"interpreter\":\"0x00000001\""),
              std::string::npos);
}

}  // namespace
}  // namespace vitreo::fuzz
