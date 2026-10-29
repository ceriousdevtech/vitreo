// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crt_stdio_binary_reads.cpp
/// @brief Binary EOF reads, the Windows read matrix and standard descriptors without a stream

#include "crt_stdio_test_fixture.hpp"

TEST_F(CrtStdioTest, BinaryFileEofPreservesBufferAndErrnoBothFamiliesAndBitnesses) {
    // Learn _read and the native binary matrix: EOF returns zero, preserving
    // errno and the caller's buffer. GuestMemory rejects zero-sized writes.
    const auto path = std::filesystem::path(tmp_path("lowio_binary_eof"));
    std::ofstream(path, std::ios::binary).close();
    for (auto flavor : {CrtDllFlavor::Msvcrt, CrtDllFlavor::Ucrt}) {
        CrtDllFlavorScope scope(flavor);
        for (bool x64 : {false, true}) {
            StdHandleRig rig(x64, path.parent_path());
            ASSERT_TRUE(rig.m.write_ansi_string(kPathAddr,
                "C:\\" + path.filename().string()).is_ok());
            const u64 fd = rig.call("_open", {kPathAddr, kO_BINARY, 0});
            ASSERT_EQ(fd, 3u);
            const u64 duplicate = rig.call("_dup", {fd});
            ASSERT_EQ(duplicate, 4u);
            ASSERT_EQ(rig.call("_dup2", {fd, 0}), 0u);
            for (const u64 descriptor : {fd, duplicate, u64{0}}) {
                std::array<u8, 32> buffer;
                buffer.fill(0xcc);
                ASSERT_TRUE(rig.memory.write_bytes(kBufAddr,
                    buffer.data(), buffer.size()).is_ok());
                rig.mm.set_errno(123);
                EXPECT_EQ(rig.call("_read", {descriptor, kBufAddr, buffer.size()}), 0u);
                EXPECT_EQ(rig.mm.get_errno(), 123u);
                ASSERT_TRUE(rig.memory.read_bytes(kBufAddr,
                    buffer.data(), buffer.size()).is_ok());
                EXPECT_TRUE(std::all_of(buffer.begin(), buffer.end(),
                    [](u8 byte) { return byte == 0xcc; }));
                EXPECT_EQ(rig.call("_close", {descriptor}), 0u);
            }
        }
    }
    std::filesystem::remove(path);
}

TEST_F(CrtStdioTest, BinaryAnsiAndUtf16ReadsMatchTheWindowsOracleMatrixBothBitnesses) {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path()
        .parent_path().parent_path();
    const auto dir = std::filesystem::path(tmp_path("unicode16")).parent_path();
    const auto path = std::filesystem::path(tmp_path("unicode16_input"));
    const std::unordered_map<std::string, std::string> inputs{
        {"ascii", std::string("A\r\nB\x1a" "C", 6)},
        {"utf16", std::string("A\0\r\0\n\0B\0\x1a\0C\0", 12)},
        {"utf8", std::string("A\0\xe2\x82\xac\xf0\x9f\x98\x80\r\nB\x1a" "C", 14)},
        {"invalid8", std::string("\xc0\xaf\xe2\x82", 4)},
        {"odd16", std::string("A\0B", 3)},
        {"bom8", std::string("\xef\xbb\xbf" "A\r\n", 6)},
        {"bom16", std::string("\xff\xfe" "A\0\r\0\n\0", 8)},
    };
    for (bool x64 : {false, true}) {
      for (const char* expected : {"crt_unicode_read_utf16.expected",
                                   "crt_unicode_read_ansi_binary.expected"}) {
        SCOPED_TRACE(expected);
        std::ifstream oracle(root / "tests/oracle_data" / expected);
        ASSERT_TRUE(oracle.good());
        std::unique_ptr<StdHandleRig> rig;
        i32 fd = -1;
        unsigned invalid_calls = 0;
        std::unique_ptr<CrtDllFlavorScope> flavor;
        std::string line;
        usize rows = 0;
        while (std::getline(oracle, line)) {
            SCOPED_TRACE(line);
            SCOPED_TRACE(x64 ? "x64" : "x86");
            std::istringstream fields(line);
            std::string dll, provider, token;
            fields >> dll >> provider;
            std::unordered_map<std::string, std::string> values;
            bool null_zero = false;
            while (fields >> token) {
                const auto equals = token.find('=');
                if (equals != std::string::npos)
                    values[token.substr(0, equals)] = token.substr(equals + 1);
                else if (token == "null-zero") null_zero = true;
            }
            const bool standard = values.at("std") == "1";
            const u32 count = null_zero ? 0u : static_cast<u32>(std::stoul(values.at("count")));
            if (null_zero || values.at("step") == "0") {
                if (rig) EXPECT_EQ(rig->call("_close", {static_cast<u32>(fd)}), 0u);
                rig.reset();
                flavor.reset();
                flavor = std::make_unique<CrtDllFlavorScope>(
                    dll == "msvcrt.dll" ? CrtDllFlavor::Msvcrt : CrtDllFlavor::Ucrt);
                rig = std::make_unique<StdHandleRig>(x64, dir);
                if (dll == "ucrtbase.dll") {
                    rig->mm.exchange_invalid_parameter_handler(0x12800);
                    rig->mm.set_guest_caller([&](GuestAddr, const u64*, usize) -> u64 {
                        ++invalid_calls; return 0;
                    });
                }
                Handle handle;
                if (provider == "pipe") {
                    Handle writer;
                    ASSERT_TRUE(rig->files.create_anonymous_pipe(handle, writer, false));
                    ASSERT_TRUE(rig->files.write_handle_bytes(writer, inputs.at(values.at("input"))));
                    ASSERT_TRUE(rig->files.close_file(writer));
                } else {
                    std::ofstream(path, std::ios::binary) << inputs.at(values.at("input"));
                    handle = rig->files.create_file("C:\\" + path.filename().string(),
                        GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0);
                    ASSERT_NE(handle, INVALID_HANDLE);
                }
                if (standard) {
                    ASSERT_TRUE(rig->console.set_std_handle(STD_INPUT_HANDLE, handle));
                    fd = 0;
                } else {
                    fd = static_cast<i32>(rig->call("_open_osfhandle", {handle, kO_BINARY}));
                    ASSERT_GE(fd, 3);
                }
                // fcntl.h modes exercised by the native probe, including
                // raw binary and ANSI text alongside wide text.
                const std::string& mode_name = values.at("mode");
                const u32 mode = mode_name == "binary" ? kO_BINARY :
                    mode_name == "text" ? kO_TEXT :
                    mode_name == "wtext" ? 0x10000u : 0x20000u;
                EXPECT_NE(static_cast<u32>(rig->call("_setmode", {static_cast<u32>(fd), mode})), ~u32{0});
            }
            ASSERT_TRUE(rig);
            std::array<u8, 128> buffer;
            buffer.fill(0xcc);
            ASSERT_TRUE(rig->memory.write_bytes(kBufAddr, buffer.data(), buffer.size()).is_ok());
            invalid_calls = 0;
            rig->mm.set_errno(123);
            const i32 result = static_cast<i32>(rig->call("_read", {
                static_cast<u32>(fd), null_zero ? 0u : kBufAddr, count}));
            EXPECT_EQ(result, std::stoi(values.at("got")));
            EXPECT_EQ(rig->mm.get_errno(), std::stoul(values.at("errno")));
            EXPECT_EQ(invalid_calls, std::stoul(values.at("invalid")));
            ASSERT_TRUE(rig->memory.read_bytes(kBufAddr, buffer.data(), buffer.size()).is_ok());
            if (!null_zero) {
                unsigned guard = 0;
                for (usize i = count; i < buffer.size(); ++i)
                    if (buffer[i] != 0xcc) ++guard;
                EXPECT_EQ(guard, std::stoul(values.at("guard")));
                std::string hex;
                constexpr char digits[] = "0123456789abcdef";
                for (i32 i = 0; i < result; ++i) {
                    hex += digits[buffer[static_cast<usize>(i)] >> 4];
                    hex += digits[buffer[static_cast<usize>(i)] & 15];
                }
                EXPECT_EQ(hex, values.at("data"));
            }
            ++rows;
        }
        EXPECT_EQ(rows, 2464u);
        if (rig) EXPECT_EQ(rig->call("_close", {static_cast<u32>(fd)}), 0u);
      }
    }
    std::filesystem::remove(path);
}

TEST_F(CrtStdioTest, StandardDescriptorsWithoutAHandleHaveNoStream) {
    const std::filesystem::path dir = std::filesystem::path(tmp_path("stdnone")).parent_path();
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        StdHandleRig rig(x64, dir);  // a GUI process: no std handles
        const u64 minus2 = x64 ? ~u64{1} : 0xFFFFFFFEu;
        // bare: _get_osfhandle(0..2)=-2 errno=0
        for (u64 fd : {0u, 1u, 2u}) {
            rig.mm.set_errno(0);
            EXPECT_EQ(rig.call("_get_osfhandle", {fd}), minus2) << fd;
            EXPECT_EQ(rig.mm.get_errno(), 0u) << fd;
        }
        // bare: _fileno(stdin)=-2 _fileno(stdout)=-2 _fileno(stderr)=-2
        for (u64 index : {0u, 1u, 2u}) {
            const u64 stream = rig.call("__acrt_iob_func", {index});
            EXPECT_EQ(static_cast<u32>(rig.call("_fileno", {stream})), 0xFFFFFFFEu)
                << index;
        }
        // bare: _write(1)=-1 errno=9
        ASSERT_TRUE(rig.m.write_ansi_string(kBufAddr, "x\n").is_ok());
        rig.mm.set_errno(0);
        EXPECT_EQ(static_cast<u32>(rig.call("_write", {1, kBufAddr, 2})), ~u32{0});
        EXPECT_EQ(rig.mm.get_errno(), 9u);
        // bare: printf=2 fflush=-1 errno=9 ferror=1 (the stream buffers; the
        // flush has nowhere to go).
        const u64 out_stream = rig.call("__acrt_iob_func", {1});
        ASSERT_TRUE(rig.m.write_ansi_string(kBufAddr, "y\n").is_ok());
        EXPECT_NE(static_cast<u32>(rig.call("fputs", {kBufAddr, out_stream})), ~u32{0});
        rig.mm.set_errno(0);
        EXPECT_EQ(static_cast<u32>(rig.call("fflush", {out_stream})), ~u32{0});
        EXPECT_EQ(rig.mm.get_errno(), 9u);
        EXPECT_EQ(static_cast<u32>(rig.call("ferror", {out_stream})), 1u);
        // bare: isatty1=64 errno=0
        rig.mm.set_errno(0);
        EXPECT_EQ(static_cast<u32>(rig.call("_isatty", {1})), 64u);
        EXPECT_EQ(rig.mm.get_errno(), 0u);
    }
}

TEST_F(CrtStdioTest, StreamDescriptorHandleIsReleasedByFcloseWithoutADoubleClose) {
    const std::filesystem::path path = tmp_path("streamosf");
    { std::ofstream(path, std::ios::binary) << "hello world"; }
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        OsHandleRig rig(x64, path.parent_path());
        ASSERT_TRUE(rig.m.write_ansi_string(
            kPathAddr, "C:\\" + path.filename().string()).is_ok());
        ASSERT_TRUE(rig.m.write_ansi_string(kModeAddr, "rb").is_ok());
        const u64 stream = rig.call("fopen", {kPathAddr, kModeAddr});
        ASSERT_NE(stream, 0u);
        const u64 fd = rig.call("_fileno", {stream});
        const u64 h = rig.call("_get_osfhandle", {fd});
        ASSERT_FALSE(rig.is_invalid_handle(h));
        EXPECT_EQ(rig.handles.get_type(static_cast<Handle>(h)), HandleType::File);
        // The stream still reads: the adopted handle does not own its file.
        EXPECT_EQ(rig.call("fread", {kBufAddr, 1, 5, stream}), 5u);
        EXPECT_EQ(rig.text(kBufAddr, 5), "hello");
        EXPECT_EQ(static_cast<u32>(rig.call("fclose", {stream})), 0u);
        EXPECT_EQ(rig.handles.get_type(static_cast<Handle>(h)),
                  HandleType::Invalid);
    }
    std::filesystem::remove(path);
}
