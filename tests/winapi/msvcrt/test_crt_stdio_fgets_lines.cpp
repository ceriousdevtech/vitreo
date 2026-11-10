// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crt_stdio_fgets_lines.cpp
/// @brief fgets over a text stream: CR-LF becomes LF, a lone CR stays, a line
///        ends after its '\n' or when n-1 characters are stored (Learn fgets;
///        fopen "t": "CR-LF combinations are translated into a single
///        linefeed character on input"). Covers a CR-LF split across the 64 KiB
///        read-ahead block and fgets mixed with fgetc/ungetc.

#include "crt_stdio_test_fixture.hpp"

namespace {
/// Windows text-mode fgets over @p raw with buffer size @p n.
std::vector<std::string> reference_lines(const std::string& raw, usize n) {
    std::string text;
    for (usize i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') continue;
        text.push_back(raw[i]);
    }
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        cur.push_back(c);
        if (c == '\n' || cur.size() == n - 1) {
            lines.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}
}  // namespace

TEST_F(CrtStdioTest, FgetsTextLinesMatchTextModeAcrossTheReadAheadBlock) {
    const auto path = std::filesystem::path(tmp_path("fgets_lines"));
    std::string raw = "Armada2\r\n\r\n Timestamp is 3be38491\r\nlone\rcr\r\n";
    // A CR-LF whose CR is the last byte of the first 64 KiB block.
    raw += std::string(65535 - raw.size(), 'x');
    raw += "\r\nafter the block\r\n";
    for (int i = 0; i < 500; ++i) raw += " 0001:00000000 ?symbol" + std::to_string(i) + "@@YAXXZ 00401000 f a.obj\r\n";
    raw += "no newline at end";
    std::ofstream(path, std::ios::binary) << raw;

    for (bool x64 : {false, true}) {
        for (const usize n : {usize{8}, usize{4096}}) {
            SCOPED_TRACE(x64 ? "x64" : "x86");
            SCOPED_TRACE(n);
            StdHandleRig rig(x64, path.parent_path());
            ASSERT_TRUE(rig.m.write_ansi_string(kPathAddr, "C:\\" + path.filename().string()).is_ok());
            ASSERT_TRUE(rig.m.write_ansi_string(kModeAddr, "rt").is_ok());
            const u64 stream = rig.call("fopen", {kPathAddr, kModeAddr});
            ASSERT_NE(stream, 0u);
            const auto expected = reference_lines(raw, n);
            usize index = 0;
            for (;;) {
                const u64 r = rig.call("fgets", {kBufAddr, n, stream});
                if (r == 0) break;
                EXPECT_EQ(r, kBufAddr);
                auto line = rig.m.read_ansi_string(kBufAddr);
                ASSERT_TRUE(line.is_ok());
                ASSERT_LT(index, expected.size());
                ASSERT_EQ(line.value(), expected[index]) << "line " << index;
                ++index;
            }
            EXPECT_EQ(index, expected.size());
            EXPECT_NE(rig.call("feof", {stream}), 0u);
            EXPECT_EQ(rig.call("fclose", {stream}), 0u);
        }
    }
    std::filesystem::remove(path);
}

TEST_F(CrtStdioTest, FgetsSharesThePositionWithFgetcAndUngetc) {
    const auto path = std::filesystem::path(tmp_path("fgets_mixed"));
    std::ofstream(path, std::ios::binary) << "abc\r\ndef\r\nghi\r\n";
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        StdHandleRig rig(x64, path.parent_path());
        ASSERT_TRUE(rig.m.write_ansi_string(kPathAddr, "C:\\" + path.filename().string()).is_ok());
        ASSERT_TRUE(rig.m.write_ansi_string(kModeAddr, "rt").is_ok());
        const u64 stream = rig.call("fopen", {kPathAddr, kModeAddr});
        ASSERT_NE(stream, 0u);
        EXPECT_EQ(rig.call("fgetc", {stream}), u64{'a'});
        ASSERT_EQ(rig.call("fgets", {kBufAddr, 64, stream}), kBufAddr);
        EXPECT_EQ(rig.m.read_ansi_string(kBufAddr).value(), "bc\n");
        EXPECT_EQ(rig.call("ungetc", {'Z', stream}), u64{'Z'});
        ASSERT_EQ(rig.call("fgets", {kBufAddr, 64, stream}), kBufAddr);
        EXPECT_EQ(rig.m.read_ansi_string(kBufAddr).value(), "Zdef\n");
        ASSERT_EQ(rig.call("fgets", {kBufAddr, 64, stream}), kBufAddr);
        EXPECT_EQ(rig.m.read_ansi_string(kBufAddr).value(), "ghi\n");
        EXPECT_EQ(rig.call("fgets", {kBufAddr, 64, stream}), 0u);
        EXPECT_EQ(rig.call("fclose", {stream}), 0u);
    }
    std::filesystem::remove(path);
}


// VC6-era stdio.h defines feof/ferror as macros over the FILE:
//   #define feof(_stream)   ((_stream)->_flag & _IOEOF)    // _IOEOF 0x0010
//   #define ferror(_stream) ((_stream)->_flag & _IOERR)    // _IOERR 0x0020
// so a legacy msvcrt stream must carry EOF and error in its _flag cell
// (struct _iobuf: +0x0C on x86, +24 on x64). A loop
// `while (!feof(f)) fgets(...)` otherwise never ends.
TEST_F(CrtStdioTest, LegacyFileFlagCarriesEofAndErrorForFeofMacros) {
    const auto path = std::filesystem::path(tmp_path("fgets_eof_flag"));
    std::ofstream(path, std::ios::binary) << "one\r\ntwo\r\n";
    CrtDllFlavorScope flavor(CrtDllFlavor::Msvcrt);
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        const u64 flag_offset = x64 ? 24 : 0x0C;
        StdHandleRig rig(x64, path.parent_path());
        ASSERT_TRUE(rig.m.write_ansi_string(kPathAddr, "C:\\" + path.filename().string()).is_ok());
        ASSERT_TRUE(rig.m.write_ansi_string(kModeAddr, "rt").is_ok());
        const u64 stream = rig.call("fopen", {kPathAddr, kModeAddr});
        ASSERT_NE(stream, 0u);
        const auto flag = [&] { return rig.memory.read_u32(stream + flag_offset).value(); };
        int lines = 0;
        while ((flag() & 0x10) == 0) {  // the feof macro
            if (rig.call("fgets", {kBufAddr, 64, stream}) != 0) ++lines;
            ASSERT_LT(lines, 10);
        }
        EXPECT_EQ(lines, 2);
        EXPECT_NE(rig.call("feof", {stream}), 0u);
        EXPECT_EQ(rig.call("fseek", {stream, 0, 0}), 0u);  // a seek clears EOF
        EXPECT_EQ(flag() & 0x10, 0u);
        EXPECT_EQ(rig.call("feof", {stream}), 0u);
        EXPECT_EQ(rig.call("fclose", {stream}), 0u);

        // Reading a write-only stream sets the error indicator; clearerr
        // clears both.
        ASSERT_TRUE(rig.m.write_ansi_string(kModeAddr, "a").is_ok());
        const u64 out = rig.call("fopen", {kPathAddr, kModeAddr});
        ASSERT_NE(out, 0u);
        const auto out_flag = [&] { return rig.memory.read_u32(out + flag_offset).value(); };
        EXPECT_EQ(rig.call("fgets", {kBufAddr, 64, out}), 0u);
        EXPECT_NE(out_flag() & 0x20, 0u);
        EXPECT_NE(rig.call("ferror", {out}), 0u);
        rig.call("clearerr", {out});
        EXPECT_EQ(out_flag() & 0x30, 0u);
        EXPECT_EQ(rig.call("fclose", {out}), 0u);
    }
    std::filesystem::remove(path);
}
