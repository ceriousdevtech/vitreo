/// @file test_string_conv.cpp
/// @brief Unit tests for MultiByteToWideChar / WideCharToMultiByte (WIN32-034)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/locale/string_conv.hpp>
#include <winapi/kernel32/locale/codepage_convert.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <vector>
#include <string>
#include <cstring>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class StringConvTest : public ::testing::Test {
protected:
    void SetUp() override {
        set_last_error(error_codes::ERROR_SUCCESS);
    }
};

// =============================================================================
// MultiByteToWideChar Tests
// =============================================================================

TEST_F(StringConvTest, AsciiRoundTrip) {
    const char* input = "Hello";
    int len = static_cast<int>(std::strlen(input));

    // UTF-8 → UTF-16
    std::vector<u16> wide(static_cast<usize>(len));
    int written = StringConverter::multi_byte_to_wide_char(
        CP_UTF8, 0, input, len, wide.data(), len);
    EXPECT_EQ(written, len);

    // UTF-16 → UTF-8
    std::vector<char> narrow(static_cast<usize>(len));
    int bytes = StringConverter::wide_char_to_multi_byte(
        CP_UTF8, 0, wide.data(), len, narrow.data(), len, nullptr, nullptr);
    EXPECT_EQ(bytes, len);
    EXPECT_EQ(std::string(narrow.data(), static_cast<usize>(bytes)), "Hello");
}

TEST_F(StringConvTest, HelloWorldUtf16RoundTrip) {
    const char* input = "Hello, World!";
    int len = static_cast<int>(std::strlen(input));

    std::vector<u16> wide(static_cast<usize>(len));
    int written = StringConverter::multi_byte_to_wide_char(
        CP_UTF8, 0, input, len, wide.data(), len);
    ASSERT_EQ(written, len);

    // Verify individual code units match ASCII values
    for (int i = 0; i < len; ++i) {
        EXPECT_EQ(wide[static_cast<usize>(i)],
                  static_cast<u16>(input[i])) << "mismatch at index " << i;
    }

    // Convert back
    std::vector<char> narrow(static_cast<usize>(len));
    int bytes = StringConverter::wide_char_to_multi_byte(
        CP_UTF8, 0, wide.data(), len, narrow.data(), len, nullptr, nullptr);
    EXPECT_EQ(bytes, len);
    EXPECT_EQ(std::string(narrow.data(), static_cast<usize>(bytes)),
              "Hello, World!");
}

TEST_F(StringConvTest, EmptyStringConversion) {
    const char* input = "";
    // 0 bytes of input → 0 wide chars
    int result = StringConverter::multi_byte_to_wide_char(
        CP_UTF8, 0, input, 0, nullptr, 0);
    EXPECT_EQ(result, 0);
}

TEST_F(StringConvTest, NullTerminatedIncludesNull) {
    const char* input = "AB";
    // cb_multibyte == -1 → strlen("AB") + 1 = 3 bytes (including null)
    int required = StringConverter::multi_byte_to_wide_char(
        CP_UTF8, 0, input, -1, nullptr, 0);
    EXPECT_EQ(required, 3); // 'A', 'B', '\0'

    std::vector<u16> wide(static_cast<usize>(required));
    int written = StringConverter::multi_byte_to_wide_char(
        CP_UTF8, 0, input, -1, wide.data(), required);
    EXPECT_EQ(written, 3);
    EXPECT_EQ(wide[0], u16('A'));
    EXPECT_EQ(wide[1], u16('B'));
    EXPECT_EQ(wide[2], u16(0));
}

TEST_F(StringConvTest, QueryRequiredSizeWithNullBuffer) {
    const char* input = "test";
    int len = static_cast<int>(std::strlen(input));

    int required = StringConverter::multi_byte_to_wide_char(
        CP_UTF8, 0, input, len, nullptr, 0);
    EXPECT_EQ(required, 4);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);
}

TEST_F(StringConvTest, InsufficientBufferSetsError) {
    const char* input = "Hello";
    int len = static_cast<int>(std::strlen(input));

    u16 tiny[2];
    int result = StringConverter::multi_byte_to_wide_char(
        CP_UTF8, 0, input, len, tiny, 2);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
}

TEST_F(StringConvTest, UnsupportedCodePageReturnsZero) {
    const char* input = "x";
    int result = StringConverter::multi_byte_to_wide_char(
        12345, 0, input, 1, nullptr, 0);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(StringConvTest, TwoByteUtf8Character) {
    // é = U+00E9 → UTF-8: 0xC3 0xA9  →  UTF-16: 0x00E9
    const char input[] = {'\xC3', '\xA9', '\0'};
    int byte_count = 2;

    int required = StringConverter::multi_byte_to_wide_char(
        CP_UTF8, 0, input, byte_count, nullptr, 0);
    EXPECT_EQ(required, 1);

    u16 wide = 0;
    int written = StringConverter::multi_byte_to_wide_char(
        CP_UTF8, 0, input, byte_count, &wide, 1);
    EXPECT_EQ(written, 1);
    EXPECT_EQ(wide, u16(0x00E9));
}

TEST_F(StringConvTest, ThreeByteUtf8CJK) {
    // 中 = U+4E2D → UTF-8: 0xE4 0xB8 0xAD → UTF-16: 0x4E2D
    const char input[] = {'\xE4', '\xB8', '\xAD', '\0'};
    int byte_count = 3;

    int required = StringConverter::multi_byte_to_wide_char(
        CP_UTF8, 0, input, byte_count, nullptr, 0);
    EXPECT_EQ(required, 1);

    u16 wide = 0;
    int written = StringConverter::multi_byte_to_wide_char(
        CP_UTF8, 0, input, byte_count, &wide, 1);
    EXPECT_EQ(written, 1);
    EXPECT_EQ(wide, u16(0x4E2D));
}

TEST_F(StringConvTest, FourByteUtf8Emoji) {
    // 😀 = U+1F600 → UTF-8: F0 9F 98 80 → UTF-16: D83D DE00 (surrogate pair)
    const char input[] = {'\xF0', '\x9F', '\x98', '\x80', '\0'};
    int byte_count = 4;

    int required = StringConverter::multi_byte_to_wide_char(
        CP_UTF8, 0, input, byte_count, nullptr, 0);
    EXPECT_EQ(required, 2); // surrogate pair = 2 code units

    u16 wide[2] = {0, 0};
    int written = StringConverter::multi_byte_to_wide_char(
        CP_UTF8, 0, input, byte_count, wide, 2);
    EXPECT_EQ(written, 2);
    EXPECT_EQ(wide[0], u16(0xD83D));
    EXPECT_EQ(wide[1], u16(0xDE00));
}

TEST_F(StringConvTest, SurrogatePairUtf16ToUtf8RoundTrip) {
    // Start with surrogate pair for U+1F600
    u16 wide[2] = {0xD83D, 0xDE00};

    // Query required UTF-8 size
    int required = StringConverter::wide_char_to_multi_byte(
        CP_UTF8, 0, wide, 2, nullptr, 0, nullptr, nullptr);
    EXPECT_EQ(required, 4); // 4-byte UTF-8 sequence

    std::vector<char> narrow(static_cast<usize>(required));
    int bytes = StringConverter::wide_char_to_multi_byte(
        CP_UTF8, 0, wide, 2, narrow.data(), required, nullptr, nullptr);
    EXPECT_EQ(bytes, 4);

    // Verify bytes: F0 9F 98 80
    EXPECT_EQ(static_cast<u8>(narrow[0]), u8(0xF0));
    EXPECT_EQ(static_cast<u8>(narrow[1]), u8(0x9F));
    EXPECT_EQ(static_cast<u8>(narrow[2]), u8(0x98));
    EXPECT_EQ(static_cast<u8>(narrow[3]), u8(0x80));

    // Round-trip back to UTF-16
    u16 rt[2] = {0, 0};
    int written = StringConverter::multi_byte_to_wide_char(
        CP_UTF8, 0, narrow.data(), 4, rt, 2);
    EXPECT_EQ(written, 2);
    EXPECT_EQ(rt[0], u16(0xD83D));
    EXPECT_EQ(rt[1], u16(0xDE00));
}

// =============================================================================
// WideCharToMultiByte Tests
// =============================================================================

TEST_F(StringConvTest, WideCharQuerySize) {
    u16 wide[] = {u16('H'), u16('i')};
    int required = StringConverter::wide_char_to_multi_byte(
        CP_UTF8, 0, wide, 2, nullptr, 0, nullptr, nullptr);
    EXPECT_EQ(required, 2);
}

TEST_F(StringConvTest, WideCharInsufficientBuffer) {
    u16 wide[] = {u16('A'), u16('B'), u16('C')};
    char tiny[1];
    int result = StringConverter::wide_char_to_multi_byte(
        CP_UTF8, 0, wide, 3, tiny, 1, nullptr, nullptr);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
}

TEST_F(StringConvTest, WideCharInsufficientBufferDoesNotTruncateCrashpadHandler) {
    const char kType[] = "crashpad-handler";
    u16 wide[17]{};
    for (int i = 0; kType[i]; ++i) {
        wide[i] = static_cast<u16>(kType[i]);
    }
    char tiny[15]{};
    std::memset(tiny, 0xCC, sizeof(tiny));
    int result = StringConverter::wide_char_to_multi_byte(
        CP_UTF8, 0, wide, -1, tiny, 15, nullptr, nullptr);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(static_cast<unsigned char>(tiny[0]), 0xCCu);
}

TEST_F(StringConvTest, WideCharNullTerminated) {
    u16 wide[] = {u16('O'), u16('K'), u16(0)};
    int required = StringConverter::wide_char_to_multi_byte(
        CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    EXPECT_EQ(required, 3); // 'O', 'K', '\0'
}

TEST_F(StringConvTest, WideCharUnsupportedCodePage) {
    u16 wide[] = {u16('x')};
    int result = StringConverter::wide_char_to_multi_byte(
        999, 0, wide, 1, nullptr, 0, nullptr, nullptr);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

// =============================================================================
// Host Helper Tests
// =============================================================================

TEST_F(StringConvTest, Utf16ToUtf8Helper) {
    u16 wide[] = {u16('H'), u16('i'), u16('!')};
    std::string result = utf16_to_utf8(wide, 3);
    EXPECT_EQ(result, "Hi!");
}

TEST_F(StringConvTest, Utf16ToUtf8HelperNullTerminated) {
    u16 wide[] = {u16('A'), u16('B'), u16(0)};
    std::string result = utf16_to_utf8(wide); // default len = -1
    EXPECT_EQ(result, "AB");
}

TEST_F(StringConvTest, Utf8ToUtf16Helper) {
    auto result = utf8_to_utf16("abc");
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], u16('a'));
    EXPECT_EQ(result[1], u16('b'));
    EXPECT_EQ(result[2], u16('c'));
}

TEST_F(StringConvTest, Utf8ToUtf16HelperEmpty) {
    auto result = utf8_to_utf16("");
    EXPECT_TRUE(result.empty());
}

TEST_F(StringConvTest, Utf8ToUtf16HelperMultibyte) {
    // é = U+00E9
    auto result = utf8_to_utf16("\xC3\xA9");
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], u16(0x00E9));
}

// =============================================================================
// CP_ACP Treated As CP_UTF8
// =============================================================================

TEST_F(StringConvTest, CpAcpSameAsUtf8) {
    const char* input = "Hello";
    int len = static_cast<int>(std::strlen(input));

    int result_acp = StringConverter::multi_byte_to_wide_char(
        CP_ACP, 0, input, len, nullptr, 0);
    int result_utf8 = StringConverter::multi_byte_to_wide_char(
        CP_UTF8, 0, input, len, nullptr, 0);
    EXPECT_EQ(result_acp, result_utf8);
    EXPECT_EQ(result_acp, 5);
}

// =============================================================================
// Export Table Test
// =============================================================================

TEST_F(StringConvTest, ExportTableHasTwoEntries) {
    DllExports exports = make_string_conv_exports();
    ASSERT_EQ(exports.entries.size(), 2u);
    EXPECT_EQ(exports.entries[0].name, "MultiByteToWideChar");
    EXPECT_EQ(exports.entries[0].ordinal, 1u);
    EXPECT_EQ(exports.entries[1].name, "WideCharToMultiByte");
    EXPECT_EQ(exports.entries[1].ordinal, 2u);
}

// =============================================================================
// NullPtr Input Handling
// =============================================================================

TEST_F(StringConvTest, NullInputMultiByte) {
    int result = StringConverter::multi_byte_to_wide_char(
        CP_UTF8, 0, nullptr, 5, nullptr, 0);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(StringConvTest, NullInputWideChar) {
    int result = StringConverter::wide_char_to_multi_byte(
        CP_UTF8, 0, nullptr, 5, nullptr, 0, nullptr, nullptr);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

// Audit 06 L5 — Windows mbwc_arg_probe.expected
TEST_F(StringConvTest, ArgChecksMatchWindows) {
    char hello[] = "hi";
    u16 wide_hi[] = {u'h', u'i', 0};
    u16 out_w[8]{};
    char out_a[8]{};

    set_last_error(0);
    EXPECT_EQ(StringConverter::multi_byte_to_wide_char(
                  CP_UTF8, 0, hello, 0, out_w, 8),
              0);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    set_last_error(0);
    EXPECT_EQ(StringConverter::multi_byte_to_wide_char(
                  CP_UTF8, 0, hello, -5, out_w, 8),
              3);  // any negative → null-terminated "hi\0"

    set_last_error(0);
    EXPECT_EQ(StringConverter::multi_byte_to_wide_char(
                  CP_UTF8, 0, hello, 2, nullptr, 8),
              0);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    char overlap[64]{};
    overlap[0] = 'a';
    set_last_error(0);
    EXPECT_EQ(StringConverter::multi_byte_to_wide_char(
                  CP_ACP, 0, overlap, 1,
                  reinterpret_cast<u16*>(overlap), 16),
              0);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    set_last_error(0);
    EXPECT_EQ(StringConverter::wide_char_to_multi_byte(
                  CP_UTF8, 0, wide_hi, 0, out_a, 8, nullptr, nullptr),
              0);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    set_last_error(0);
    EXPECT_EQ(StringConverter::wide_char_to_multi_byte(
                  CP_UTF8, 0, wide_hi, 2, nullptr, 8, nullptr, nullptr),
              0);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    set_last_error(0);
    EXPECT_EQ(StringConverter::wide_char_to_multi_byte(
                  CP_UTF8, 0, wide_hi, 2, nullptr, 0, nullptr, nullptr),
              2);  // size query
}

TEST_F(StringConvTest, MbCompositeAndUseGlyphChars) {
    // Windows: MB_COMPOSITE on 1252 0xE9 → U+0065 U+0301; USEGLYPH on 0xE9
    // is identity U+00E9; USEGLYPH on 437 0x01 → U+263A.
    u16 out[4]{};
    auto r = codepage::multi_byte_to_wide(1252, 0x2 /*MB_COMPOSITE*/,
                                          reinterpret_cast<const u8*>("\xE9"),
                                          1, out, 4);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.count, 2u);
    EXPECT_EQ(out[0], 0x0065u);
    EXPECT_EQ(out[1], 0x0301u);

    r = codepage::multi_byte_to_wide(1252, 0x4 /*MB_USEGLYPHCHARS*/,
                                     reinterpret_cast<const u8*>("\xE9"), 1,
                                     out, 4);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.count, 1u);
    EXPECT_EQ(out[0], 0x00E9u);

    r = codepage::multi_byte_to_wide(437, 0x4, reinterpret_cast<const u8*>("\x01"),
                                     1, out, 4);
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.count, 1u);
    EXPECT_EQ(out[0], 0x263Au);
}

// =============================================================================
// Mixed content: ASCII + multibyte in one string
// =============================================================================

TEST_F(StringConvTest, MixedAsciiAndMultibyte) {
    // "café" = 'c' 'a' 'f' 0xC3 0xA9  (5 bytes → 4 wide chars)
    const char* input = "caf\xC3\xA9";
    int byte_count = 5;

    int required = StringConverter::multi_byte_to_wide_char(
        CP_UTF8, 0, input, byte_count, nullptr, 0);
    EXPECT_EQ(required, 4); // c a f é

    std::vector<u16> wide(static_cast<usize>(required));
    int written = StringConverter::multi_byte_to_wide_char(
        CP_UTF8, 0, input, byte_count, wide.data(), required);
    EXPECT_EQ(written, 4);
    EXPECT_EQ(wide[0], u16('c'));
    EXPECT_EQ(wide[1], u16('a'));
    EXPECT_EQ(wide[2], u16('f'));
    EXPECT_EQ(wide[3], u16(0x00E9));

    // Round-trip back
    std::vector<char> narrow(static_cast<usize>(byte_count));
    int bytes = StringConverter::wide_char_to_multi_byte(
        CP_UTF8, 0, wide.data(), 4, narrow.data(), byte_count,
        nullptr, nullptr);
    EXPECT_EQ(bytes, 5);
    EXPECT_EQ(std::string(narrow.data(), 5), "caf\xC3\xA9");
}

TEST(StringConvThunkTest, WideCharToMultiByteFailsInsteadOfTruncating) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x10000, 0x2000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Long;
    cpu.gpr.rsp = 0x11000;
    const char kType[] = "crashpad-handler";
    constexpr GuestAddr wide_addr = 0x10000;
    constexpr GuestAddr dest_addr = 0x10800;
    for (u32 i = 0; kType[i]; ++i) {
        ASSERT_TRUE(memory.write_u16(wide_addr + i * 2,
                                     static_cast<u16>(kType[i])).is_ok());
    }
    ASSERT_TRUE(memory.write_u16(wide_addr + 16 * 2, 0).is_ok());
    for (u32 i = 0; i < 15; ++i) {
        ASSERT_TRUE(memory.write_u8(dest_addr + i, 0xCC).is_ok());
    }
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, dest_addr).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, 15).is_ok());
    cpu.gpr.rcx = CP_UTF8;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = wide_addr;
    cpu.gpr.r9 = static_cast<u64>(static_cast<i64>(-1));
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    set_last_error(error_codes::ERROR_SUCCESS);
    impl_WideCharToMultiByte(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    // Measured on Windows (tools/oracle_probe/string_conversion_probe.expected,
    // "utf8-small"): the call fails, but the bytes that fit are written. What
    // must never happen is a success return for a truncated string.
    EXPECT_EQ(memory.read_u8(dest_addr).value(), static_cast<u8>('c'));
}

// Audit 06 L2: GetCommandLineA / utf8_to_acp must apply CP1252 best-fit
// (Windows cp1252_bestfit_probe: U+0100→'A', U+03B1→'a', U+4E00→'?').
TEST_F(StringConvTest, Utf8ToAcpAppliesCp1252BestFit) {
    // U+0100 as UTF-8 C4 80
    EXPECT_EQ(utf8_to_acp("\xC4\x80"), "A");
    // U+0101
    EXPECT_EQ(utf8_to_acp("\xC4\x81"), "a");
    // U+03B1 greek alpha
    EXPECT_EQ(utf8_to_acp("\xCE\xB1"), "a");
    // U+4E00 CJK → default '?'
    EXPECT_EQ(utf8_to_acp("\xE4\xB8\x80"), "?");
    // Mix matching the probe string Āα一
    EXPECT_EQ(utf8_to_acp("\xC4\x80\xCE\xB1\xE4\xB8\x80"), "Aa?");
    // Direct CP1252 euro U+20AC
    EXPECT_EQ(utf8_to_acp("\xE2\x82\xAC"), "\x80");
}
