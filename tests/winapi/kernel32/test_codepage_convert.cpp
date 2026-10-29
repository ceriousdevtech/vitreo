// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_codepage_convert.cpp
/// @brief MultiByteToWideChar / WideCharToMultiByte against Windows' own output.
///
/// Every expected value here was produced by Windows (tools/oracle_probe/*
/// .expected); none is derived from Vitreo.

#include "winapi/kernel32/locale/codepage_convert.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi::codepage;

namespace {

std::string hex(const std::vector<u8>& v) {
    std::string s; char b[3];
    for (u8 x : v) { std::snprintf(b, sizeof(b), "%02x", x); s += b; }
    return s;
}
std::string hex16(const u16* v, usize n) {
    std::string s; char b[5];
    for (usize i = 0; i < n; ++i) { std::snprintf(b, sizeof(b), "%04x", v[i]); s += b; }
    return s;
}

// The generator of tools/oracle_probe/utf8_corpus_probe.c, verbatim.
struct Xorshift {
    u32 s = 0x9E3779B9u;
    u32 next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
};

} // namespace

TEST(CodepageConvert, Utf8CorpusMatchesWindowsExactly) {
    std::ifstream expected(VITREO_ORACLE_PROBE_DIR "/utf8_corpus_probe.expected");
    ASSERT_TRUE(expected.good());
    Xorshift rng;
    std::string line;
    usize checked = 0;
    char buf[32];
    for (int c = 0; c < 12000; ++c) {
        u8 in[8]; const u32 n = 1 + rng.next() % 8;
        for (u32 i = 0; i < n; ++i) {
            const u32 r = rng.next() % 16;
            in[i] = r < 3 ? static_cast<u8>(0x20 + rng.next() % 0x60)
                  : r < 9 ? static_cast<u8>(0x80 + rng.next() % 0x40)
                          : static_cast<u8>(0xC0 + rng.next() % 0x40);
        }
        for (int f = 0; f < 2; ++f) {
            u16 w[32];
            const auto r = multi_byte_to_wide(kCpUtf8, f ? 0x8 : 0, in, n, w, 32);
            std::string got = std::string("M") + char('0' + f) + " " +
                              hex(std::vector<u8>(in, in + n)) + " ";
            std::snprintf(buf, sizeof(buf), "%02x %04x ", r.count, r.ok() ? 0 : r.error);
            got += buf;
            got += hex16(w, r.ok() ? r.count : 0);
            ASSERT_TRUE(std::getline(expected, line));
            ASSERT_EQ(got, line);
            ++checked;
        }
    }
    for (int c = 0; c < 8000; ++c) {
        u16 in[6]; const u32 n = 1 + rng.next() % 6;
        for (u32 i = 0; i < n; ++i) {
            const u32 r = rng.next() % 8;
            in[i] = static_cast<u16>(r < 2 ? 0x20 + rng.next() % 0x60
                                   : r < 4 ? 0xD800 + rng.next() % 0x800
                                   : r < 5 ? 0x80 + rng.next() % 0x780
                                           : rng.next() % 0x10000);
        }
        for (int f = 0; f < 2; ++f) {
            u8 b[64];
            const auto r = wide_to_multi_byte(kCpUtf8, f ? 0x80 : 0, in, n, b, 64,
                                              nullptr, false);
            std::string got = std::string("W") + char('0' + f) + " " + hex16(in, n) + " ";
            std::snprintf(buf, sizeof(buf), "%02x %04x ", r.count, r.ok() ? 0 : r.error);
            got += buf;
            got += hex(std::vector<u8>(b, b + (r.ok() ? r.count : 0)));
            ASSERT_TRUE(std::getline(expected, line));
            ASSERT_EQ(got, line);
            ++checked;
        }
    }
    EXPECT_EQ(checked, 40000u);
}

// tools/oracle_probe/string_conversion_probe.expected, case by case.
TEST(CodepageConvert, EdgeCasesMatchWindows) {
    const u8 hello[] = {'h', 0xC3, 0xA9, 'l', 'l', 'o', 0};
    u16 w[16] = {};
    auto r = multi_byte_to_wide(kCpUtf8, 0, hello, 7, nullptr, 0);   // utf8-query-ntz
    EXPECT_EQ(r.count, 6u);
    r = multi_byte_to_wide(kCpUtf8, 0, hello, 7, w, 3);                // utf8-small-ntz
    EXPECT_EQ(r.error, 122u);
    EXPECT_EQ(w[0], 0x0068);
    const u8 emoji[] = {'a', 0xF0, 0x9F, 0x98, 0x80, 'z'};
    r = multi_byte_to_wide(kCpUtf8, 0, emoji, 6, w, 2);                 // utf8-emoji-short2
    EXPECT_EQ(r.error, 122u);
    r = multi_byte_to_wide(kCpUtf8, 0x1, hello, 7, w, 16);              // utf8-flag-precomp
    EXPECT_EQ(r.count, 6u);
    r = multi_byte_to_wide(kCpUtf8, 0x10, hello, 7, w, 16);             // flags matrix
    EXPECT_EQ(r.error, 1004u);
    const u8 hi[] = {0x80, 0x81, 0x8D, 0x8F, 0x90, 0x9D, 0xA0, 0xFF};    // cp1252-all-hi
    r = multi_byte_to_wide(1252, 0, hi, 8, w, 16);
    ASSERT_EQ(r.count, 8u);
    EXPECT_EQ(hex16(w, 8), "20ac0081008d008f0090009d00a000ff");
    const u8 oem[] = {0x80, 0xB0, 0xE1, 0xFE};                           // cp437-hi
    r = multi_byte_to_wide(437, 0, oem, 4, w, 16);
    EXPECT_EQ(hex16(w, 4), "00c7259100df25a0");
    const u8 e9 = 0xE9, x82 = 0x82, a = 0x41;
    r = multi_byte_to_wide(kCpOemcp, 0, &x82, 1, w, 16);                // cp-oemcp
    EXPECT_EQ(w[0], 0x00E9);
    r = multi_byte_to_wide(kCpThreadAcp, 0, &e9, 1, w, 16);             // cp-thread
    EXPECT_EQ(w[0], 0x00E9);
    r = multi_byte_to_wide(kCpSymbol, 0, &a, 1, w, 16);                 // cp-symbol
    EXPECT_EQ(w[0], 0xF041);
    r = multi_byte_to_wide(12345, 0, &a, 1, w, 16);                     // cp-invalid
    EXPECT_EQ(r.error, 87u);

    const u16 wlatin[] = {0x100, 0x2022, 0x3042, 'x', 0};
    u8 b[16] = {};
    r = wide_to_multi_byte(1252, 0, wlatin, 5, b, 16, nullptr, true);   // cp1252-latin
    EXPECT_EQ(r.count, 5u);
    EXPECT_TRUE(r.used_default);
    EXPECT_EQ(hex(std::vector<u8>(b, b + 5)), "41953f7800");
    r = wide_to_multi_byte(1252, 0x400, wlatin, 5, b, 16, nullptr, true); // -nbf
    EXPECT_EQ(hex(std::vector<u8>(b, b + 5)), "3f953f7800");
    const u8 hash = '#';
    r = wide_to_multi_byte(1252, 0x400, wlatin, 5, b, 16, &hash, true);  // -def
    EXPECT_EQ(hex(std::vector<u8>(b, b + 5)), "23952378" "00");
    r = wide_to_multi_byte(437, 0, wlatin, 5, b, 16, nullptr, true);     // cp437-latin
    EXPECT_EQ(hex(std::vector<u8>(b, b + 5)), "41073f7800");
    const u16 lonehi[] = {'a', 0xD83D, 'z', 0};
    r = wide_to_multi_byte(kCpUtf8, 0, lonehi, 4, b, 16, nullptr, false); // utf8-lonehi
    EXPECT_EQ(hex(std::vector<u8>(b, b + r.count)), "61efbfbd7a00");
    r = wide_to_multi_byte(kCpUtf8, 0x80, lonehi, 4, b, 16, nullptr, false);
    EXPECT_EQ(r.error, 1113u);
    EXPECT_EQ(r.written, 1u);  // utf8-lonehi-err: "61" left in the buffer
    r = wide_to_multi_byte(kCpUtf8, 0, lonehi, 4, b, 16, &hash, true);   // accepted
    EXPECT_TRUE(r.ok());
    EXPECT_FALSE(r.used_default);
    r = wide_to_multi_byte(1252, 0x80, lonehi, 4, b, 16, nullptr, false); // flags
    EXPECT_EQ(r.error, 1004u);
    r = wide_to_multi_byte(1252, 0x40, lonehi, 4, b, 16, nullptr, false);
    EXPECT_EQ(r.error, 1004u);
    r = wide_to_multi_byte(1252, 0x240, lonehi, 4, b, 16, nullptr, false);
    EXPECT_TRUE(r.ok());
    const u16 f041 = 0xF041, plain_a = 'a';
    r = wide_to_multi_byte(kCpSymbol, 0, &plain_a, 1, b, 16, nullptr, false);
    EXPECT_EQ(r.error, 1113u);
    r = wide_to_multi_byte(kCpSymbol, 0, &f041, 1, b, 16, nullptr, true);
    EXPECT_EQ(r.error, 87u);
}

// string_conversion_probe utf8-err-invalid / utf8-err-query-inv: a strict
// failure leaves the converted prefix, and writes nothing in query mode.
TEST(CodepageConvert, StrictUtf8FailureKeepsConvertedPrefix) {
    const char in[] = "a\xFF" "b";
    u16 w[16];
    auto r = multi_byte_to_wide(kCpUtf8, 0x8, reinterpret_cast<const u8*>(in), 3, w, 16);
    EXPECT_EQ(r.error, 1113u);
    ASSERT_EQ(r.written, 1u);
    EXPECT_EQ(w[0], u16{'a'});
    r = multi_byte_to_wide(kCpUtf8, 0x8, reinterpret_cast<const u8*>(in), 3, nullptr, 0);
    EXPECT_EQ(r.error, 1113u);
    EXPECT_EQ(r.written, 0u);
}

TEST(CodepageConvert, ValidityMatchesWindows) {
    // string_conversion_flags_probe.expected "V" lines, for the pages Vitreo
    // implements; UTF-7 and the DBCS pages are valid on Windows but not yet
    // implemented here, so they honestly report FALSE.
    EXPECT_TRUE(is_valid_code_page(65001));
    EXPECT_TRUE(is_valid_code_page(1252));
    EXPECT_TRUE(is_valid_code_page(437));
    EXPECT_TRUE(is_valid_code_page(20127));
    EXPECT_TRUE(is_valid_code_page(28591));
    for (u32 pseudo : {0u, 1u, 2u, 3u, 42u}) EXPECT_FALSE(is_valid_code_page(pseudo));
    EXPECT_FALSE(is_valid_code_page(12345));
}
