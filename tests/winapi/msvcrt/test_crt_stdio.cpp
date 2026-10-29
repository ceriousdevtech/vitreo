// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crt_stdio.cpp
/// @brief Unit tests for msvcrt file I/O / stdio functions (WIN32-084)
///
/// Sibling files test_crt_stdio_*.cpp hold the remaining test groups; the shared
/// fixture lives in crt_stdio_test_fixture.hpp.

#include "crt_stdio_test_fixture.hpp"

// =============================================================================
// 1. Fopen_WriteMode_ReturnsValidFd
// =============================================================================

TEST_F(CrtStdioTest, Fopen_WriteMode_ReturnsValidFd) {
    auto path = tmp_path("basic");
    i32 fd = mgr.crt_fopen(path, "w");
    ASSERT_GE(fd, 3); // 0-2 reserved for stdin/stdout/stderr
    EXPECT_NE(mgr.get_file(fd), nullptr);
    mgr.crt_fclose(fd);
}

// =============================================================================
// 2. Fopen_MissingFile_ReturnsNegOne
// =============================================================================

TEST_F(CrtStdioTest, Fopen_MissingFile_ReturnsNegOne) {
    i32 fd = mgr.crt_fopen("/nonexistent/path/file.txt", "r");
    EXPECT_EQ(fd, -1);
}

// =============================================================================
// 3. Fwrite_Fread_Roundtrip
// =============================================================================

TEST_F(CrtStdioTest, Fwrite_Fread_Roundtrip) {
    auto path = tmp_path("rw");

    // Write
    i32 wfd = mgr.crt_fopen(path, "w");
    ASSERT_GE(wfd, 3);
    const char msg[] = "hello vitreo";
    usize written = mgr.crt_fwrite(msg, 1, sizeof(msg) - 1, wfd);
    EXPECT_EQ(written, sizeof(msg) - 1);
    mgr.crt_fclose(wfd);

    // Read back
    i32 rfd = mgr.crt_fopen(path, "r");
    ASSERT_GE(rfd, 3);
    char buf[64] = {};
    usize rd = mgr.crt_fread(buf, 1, sizeof(buf), rfd);
    EXPECT_EQ(rd, sizeof(msg) - 1);
    EXPECT_STREQ(buf, "hello vitreo");
    mgr.crt_fclose(rfd);
}

// =============================================================================
// 4. Fseek_Ftell_Roundtrip
// =============================================================================

TEST_F(CrtStdioTest, Fseek_Ftell_Roundtrip) {
    auto path = tmp_path("seek");

    i32 fd = mgr.crt_fopen(path, "w+");
    ASSERT_GE(fd, 3);

    const char data[] = "ABCDEFGHIJ";
    (void)mgr.crt_fwrite(data, 1, 10, fd);

    // Seek to position 5
    EXPECT_EQ(mgr.crt_fseek(fd, 5, SEEK_SET), 0);
    EXPECT_EQ(mgr.crt_ftell(fd), 5);

    // Read from position 5
    char buf[6] = {};
    usize n = mgr.crt_fread(buf, 1, 5, fd);
    EXPECT_EQ(n, 5u);
    EXPECT_STREQ(buf, "FGHIJ");

    mgr.crt_fclose(fd);
}

// =============================================================================
// 5. Feof_AfterReadingPastEnd
// =============================================================================

TEST_F(CrtStdioTest, Feof_AfterReadingPastEnd) {
    auto path = tmp_path("eof");

    i32 wfd = mgr.crt_fopen(path, "w");
    ASSERT_GE(wfd, 3);
    (void)mgr.crt_fwrite("AB", 1, 2, wfd);
    mgr.crt_fclose(wfd);

    i32 rfd = mgr.crt_fopen(path, "r");
    ASSERT_GE(rfd, 3);

    // Not at EOF yet
    EXPECT_EQ(mgr.crt_feof(rfd), 0);

    // Read all bytes
    char buf[16] = {};
    (void)mgr.crt_fread(buf, 1, sizeof(buf), rfd);

    // Now at EOF
    EXPECT_NE(mgr.crt_feof(rfd), 0);
    mgr.crt_fclose(rfd);
}

// =============================================================================
// 6. Ferror_CleanStream_ReturnsZero
// =============================================================================

TEST_F(CrtStdioTest, Ferror_CleanStream_ReturnsZero) {
    i32 fd = mgr.crt_tmpfile();
    ASSERT_GE(fd, 3);
    EXPECT_EQ(mgr.crt_ferror(fd), 0);
    mgr.crt_fclose(fd);
}

// =============================================================================
// 7. Fclose_ReturnsZero
// =============================================================================

TEST_F(CrtStdioTest, Fclose_ReturnsZero) {
    auto path = tmp_path("basic");
    i32 fd = mgr.crt_fopen(path, "w");
    ASSERT_GE(fd, 3);
    EXPECT_EQ(mgr.crt_fclose(fd), 0);
    // After close, get_file should return nullptr
    EXPECT_EQ(mgr.get_file(fd), nullptr);
}

// =============================================================================
// 8. Fgets_ReadsLine
// =============================================================================

TEST_F(CrtStdioTest, Fgets_ReadsLine) {
    auto path = tmp_path("fgets");

    i32 wfd = mgr.crt_fopen(path, "w");
    ASSERT_GE(wfd, 3);
    mgr.crt_fputs("first line\nsecond line\n", wfd);
    mgr.crt_fclose(wfd);

    i32 rfd = mgr.crt_fopen(path, "r");
    ASSERT_GE(rfd, 3);

    char line[64] = {};
    bool ok = mgr.crt_fgets(line, sizeof(line), rfd);
    EXPECT_TRUE(ok);
    EXPECT_STREQ(line, "first line\n");

    mgr.crt_fclose(rfd);
}

// =============================================================================
// 9. Fputs_WritesString
// =============================================================================

TEST_F(CrtStdioTest, Fputs_WritesString) {
    auto path = tmp_path("fputs");

    i32 wfd = mgr.crt_fopen(path, "w");
    ASSERT_GE(wfd, 3);
    i32 rc = mgr.crt_fputs("fputs_test", wfd);
    EXPECT_GE(rc, 0);
    mgr.crt_fclose(wfd);

    // Verify
    i32 rfd = mgr.crt_fopen(path, "r");
    ASSERT_GE(rfd, 3);
    char buf[32] = {};
    (void)mgr.crt_fread(buf, 1, sizeof(buf), rfd);
    EXPECT_STREQ(buf, "fputs_test");
    mgr.crt_fclose(rfd);
}

// =============================================================================
// 10. Fputc_Fgetc_Roundtrip
// =============================================================================

TEST_F(CrtStdioTest, Fputc_Fgetc_Roundtrip) {
    auto path = tmp_path("putcgetc");

    i32 wfd = mgr.crt_fopen(path, "w");
    ASSERT_GE(wfd, 3);
    mgr.crt_fputc('X', wfd);
    mgr.crt_fputc('Y', wfd);
    mgr.crt_fclose(wfd);

    i32 rfd = mgr.crt_fopen(path, "r");
    ASSERT_GE(rfd, 3);
    EXPECT_EQ(mgr.crt_fgetc(rfd), 'X');
    EXPECT_EQ(mgr.crt_fgetc(rfd), 'Y');
    EXPECT_EQ(mgr.crt_fgetc(rfd), EOF);
    mgr.crt_fclose(rfd);
}

// =============================================================================
// 11. Ungetc_PushesBack
// =============================================================================

TEST_F(CrtStdioTest, Ungetc_PushesBack) {
    auto path = tmp_path("ungetc");

    i32 wfd = mgr.crt_fopen(path, "w");
    ASSERT_GE(wfd, 3);
    mgr.crt_fputc('A', wfd);
    mgr.crt_fputc('B', wfd);
    mgr.crt_fclose(wfd);

    i32 rfd = mgr.crt_fopen(path, "r");
    ASSERT_GE(rfd, 3);

    // Read one char
    i32 ch = mgr.crt_fgetc(rfd);
    EXPECT_EQ(ch, 'A');

    // Push it back
    mgr.crt_ungetc('Z', rfd);

    // Next read should return pushed-back char
    EXPECT_EQ(mgr.crt_fgetc(rfd), 'Z');
    // Then normal file content
    EXPECT_EQ(mgr.crt_fgetc(rfd), 'B');

    mgr.crt_fclose(rfd);
}

// =============================================================================
// 12. Fprintf_WritesFormatted
// =============================================================================

TEST_F(CrtStdioTest, Fprintf_WritesFormatted) {
    auto path = tmp_path("fprintf");

    i32 wfd = mgr.crt_fopen(path, "w");
    ASSERT_GE(wfd, 3);
    i32 n = mgr.crt_fprintf(wfd, "val=%d str=%s", 42, "ok");
    EXPECT_GT(n, 0);
    mgr.crt_fclose(wfd);

    // Verify
    i32 rfd = mgr.crt_fopen(path, "r");
    ASSERT_GE(rfd, 3);
    char buf[64] = {};
    (void)mgr.crt_fread(buf, 1, sizeof(buf), rfd);
    EXPECT_STREQ(buf, "val=42 str=ok");
    mgr.crt_fclose(rfd);
}

// =============================================================================
// 13. Fflush_Stdout_Succeeds
// =============================================================================

TEST_F(CrtStdioTest, Fflush_Stdout_Succeeds) {
    // fd 1 is stdout
    EXPECT_EQ(mgr.crt_fflush(1), 0);
}

// =============================================================================
// 14. BinaryMode_PreservesBytes
// =============================================================================

TEST_F(CrtStdioTest, BinaryMode_PreservesBytes) {
    auto path = tmp_path("binary");

    // Write raw bytes including null and high bytes
    u8 data[] = {0x00, 0xFF, 0x80, 0x01, 0xFE, 0x7F};

    i32 wfd = mgr.crt_fopen(path, "wb");
    ASSERT_GE(wfd, 3);
    usize written = mgr.crt_fwrite(data, 1, sizeof(data), wfd);
    EXPECT_EQ(written, sizeof(data));
    mgr.crt_fclose(wfd);

    i32 rfd = mgr.crt_fopen(path, "rb");
    ASSERT_GE(rfd, 3);
    u8 buf[16] = {};
    usize rd = mgr.crt_fread(buf, 1, sizeof(buf), rfd);
    EXPECT_EQ(rd, sizeof(data));
    EXPECT_EQ(::memcmp(buf, data, sizeof(data)), 0);
    mgr.crt_fclose(rfd);
}

// =============================================================================
// 15. Tmpfile_CreatesWritableFd
// =============================================================================

TEST_F(CrtStdioTest, Tmpfile_CreatesWritableFd) {
    i32 fd = mgr.crt_tmpfile();
    ASSERT_GE(fd, 3);
    EXPECT_NE(mgr.get_file(fd), nullptr);

    // Write and read back
    const char msg[] = "tmp";
    usize w = mgr.crt_fwrite(msg, 1, 3, fd);
    EXPECT_EQ(w, 3u);

    mgr.crt_fseek(fd, 0, SEEK_SET);
    char buf[8] = {};
    usize r = mgr.crt_fread(buf, 1, sizeof(buf), fd);
    EXPECT_EQ(r, 3u);
    EXPECT_STREQ(buf, "tmp");

    mgr.crt_fclose(fd);
}

// =============================================================================
// 16. Rewind_ResetsPosition
// =============================================================================

TEST_F(CrtStdioTest, Rewind_ResetsPosition) {
    i32 fd = mgr.crt_tmpfile();
    ASSERT_GE(fd, 3);

    (void)mgr.crt_fwrite("XYZ", 1, 3, fd);
    EXPECT_EQ(mgr.crt_ftell(fd), 3);

    mgr.crt_rewind(fd);
    EXPECT_EQ(mgr.crt_ftell(fd), 0);

    mgr.crt_fclose(fd);
}

// =============================================================================
// 17. Fclose_StdStreams_ReturnsError
// =============================================================================

TEST_F(CrtStdioTest, Fclose_StdStreams_ReturnsError) {
    // Closing stdin/stdout/stderr should be refused
    EXPECT_EQ(mgr.crt_fclose(0), -1);
    EXPECT_EQ(mgr.crt_fclose(1), -1);
    EXPECT_EQ(mgr.crt_fclose(2), -1);
}

// =============================================================================
// 18. GetFile_InvalidFd_ReturnsNull
// =============================================================================

TEST_F(CrtStdioTest, GetFile_InvalidFd_ReturnsNull) {
    EXPECT_EQ(mgr.get_file(-1), nullptr);
    EXPECT_EQ(mgr.get_file(999), nullptr);
}
