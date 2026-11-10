// SPDX-License-Identifier: LGPL-2.1-only
#include <gtest/gtest.h>
#include <core/compression/lzx.hpp>

using namespace vitreo;
using namespace vitreo::compression;

namespace {
// Original bitstream encoder for the MS-PATCH 2.3.1 / 2.3.2.1 wire rules.
// Expected output is the encoder input, never captured decoder output.
struct LzxBits {
    std::vector<u8> bytes;
    u16 word = 0;
    u32 used = 0;

    void put(u32 value, u32 count) {
        for (u32 n = count; n != 0; --n) {
            word = static_cast<u16>((word << 1) | ((value >> (n - 1)) & 1));
            if (++used == 16) {
                bytes.push_back(static_cast<u8>(word));
                bytes.push_back(static_cast<u8>(word >> 8));
                word = 0;
                used = 0;
            }
        }
    }
    void block(u32 type, u32 size) { put(type, 3); put(size, 24); }
    void lengths(u32 count, bool literals) {
        // Two one-bit pre-tree codes: 0 -> unchanged zero length,
        // 16 -> (0 + 17 - 16) % 17 == 1. Main codes A/B then have length 1.
        for (u32 i = 0; i < lzx::kPreTreeSymbols; ++i)
            put(i == 0 || i == 16 ? 1 : 0, 4);
        for (u32 i = 0; i < count; ++i)
            put(literals && (i == 'A' || i == 'B') ? 1 : 0, 1);
    }
    void raw_header(u32 size) {
        block(3, size);
        put(0, 16 - used); // MS-PATCH 2.3.2.1: 1..16, including aligned case.
        for (u32 i = 0; i < 3; ++i)
            bytes.insert(bytes.end(), {1, 0, 0, 0}); // R0/R1/R2, little endian.
    }
};
}

TEST(Lzx, UncompressedBlockConsumesEveryHeaderPaddingWidth) {
    // Vary the preceding literal count to exercise all 16 header positions,
    // including the already-aligned case that requires a full padding word.
    for (u32 literals = 1; literals <= 16; ++literals) {
        SCOPED_TRACE(literals);
        LzxBits bits;
        bits.put(0, 1); // No x86 translation.
        bits.block(1, literals);
        bits.lengths(256, true);
        bits.lengths(30 * 8, false); // 15-bit window position slots.
        bits.lengths(lzx::kLengthTreeSymbols, false);
        std::vector<u8> expected;
        for (u32 i = 0; i < literals; ++i) {
            bits.put(i & 1, 1);
            expected.push_back(i & 1 ? 'B' : 'A');
        }
        bits.raw_header(3);
        bits.bytes.insert(bits.bytes.end(), {'x', 'y', 'z', 0}); // Odd-length pad.
        bits.raw_header(2);
        bits.bytes.insert(bits.bytes.end(), {'p', 'q'});
        expected.insert(expected.end(), {'x', 'y', 'z', 'p', 'q'});
        auto decoder = lzx::Decoder::create(15);
        ASSERT_TRUE(decoder.is_ok());
        std::vector<u8> actual;
        auto result = decoder.value().decode_chunk(bits.bytes, expected.size(), actual);
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(actual, expected);
    }
}

TEST(Lzx, UncompressedBlockContinuesAcrossChunkBoundary) {
    LzxBits bits;
    bits.put(0, 1);
    bits.raw_header(lzx::kFrameSize + 3);
    std::vector<u8> expected(lzx::kFrameSize, 'A');
    bits.bytes.insert(bits.bytes.end(), expected.begin(), expected.end());
    auto decoder = lzx::Decoder::create(15);
    ASSERT_TRUE(decoder.is_ok());
    std::vector<u8> actual;
    ASSERT_TRUE(decoder.value().decode_chunk(bits.bytes, expected.size(), actual).is_ok());
    EXPECT_EQ(actual, expected);
    const std::vector<u8> continuation{'x', 'y', 'z', 0};
    ASSERT_TRUE(decoder.value().decode_chunk(continuation, 3, actual).is_ok());
    expected.insert(expected.end(), {'x', 'y', 'z'});
    EXPECT_EQ(actual, expected);
}

TEST(Lzx, TruncatedUncompressedHeaderFailsWithoutOutput) {
    LzxBits bits;
    bits.put(0, 1);
    bits.raw_header(2);
    bits.bytes.pop_back(); // Truncated R2.
    auto decoder = lzx::Decoder::create(15);
    ASSERT_TRUE(decoder.is_ok());
    std::vector<u8> actual;
    EXPECT_TRUE(decoder.value().decode_chunk(bits.bytes, 2, actual).is_err());
    EXPECT_TRUE(actual.empty());
}
