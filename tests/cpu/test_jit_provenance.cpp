// SPDX-License-Identifier: LGPL-2.1-only
#include <gtest/gtest.h>
#include "core/cpu/jit/jit_provenance.hpp"
#include <string>

using vitreo::cpu::jit::symbol_map::ProvenanceWriter;

TEST(JitProvenance, WholeRecordsCapAndFlushWatermark) {
    auto* file = std::tmpfile();
    ASSERT_NE(file, nullptr);
    ProvenanceWriter writer(file, 2, 7, 10);
    writer.record(20, 0x1000, 0x2000, 16, 0x3000, 8);
    writer.record(30, 0x1004, 0x2000, 16, 0, 0); // retain reused owner
    writer.record(40, 0x1008, 0x4000, 16, 0, 0);
    writer.record(50, 0x1010, 0x5000, 16, 0, 0);
    EXPECT_TRUE(writer.lost());
    EXPECT_EQ(writer.count(), 2u);
    ASSERT_TRUE(writer.flush(60));
    std::rewind(file);
    char bytes[1024]{};
    const auto count = std::fread(bytes, 1, sizeof(bytes), file);
    EXPECT_EQ(std::string(bytes, count),
        "V1 7 10 2\nR 1 20 1000 2000 10 3000 8\n"
        "R 2 30 1004 2000 10 0 0\nLOSS cap 40\nF 2 60 1\n");
    std::fclose(file);
}

TEST(JitProvenance, FailedStreamCannotReportSuccessfulFlush) {
    auto* file = std::fopen("/dev/null", "r");
    ASSERT_NE(file, nullptr);
    ProvenanceWriter writer(file, 2, 7, 10);
    writer.record(20, 0x1000, 0x2000, 16, 0, 0);
    EXPECT_FALSE(writer.flush(30));
    EXPECT_TRUE(writer.failed());
    std::fclose(file);
}
