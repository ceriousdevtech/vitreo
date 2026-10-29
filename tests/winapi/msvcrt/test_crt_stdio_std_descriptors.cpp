// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crt_stdio_std_descriptors.cpp
/// @brief Standard-descriptor pipe binding and per-DLL CRT arenas

#include "crt_stdio_test_fixture.hpp"

TEST_F(CrtStdioTest, StandardDescriptorsWriteThroughAnInheritedPipe) {
    const std::filesystem::path dir = std::filesystem::path(tmp_path("stdpipe")).parent_path();
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        StdHandleRig rig(x64, dir);
        Handle read_end = 0;
        Handle write_end = 0;
        ASSERT_TRUE(rig.files.create_anonymous_pipe(read_end, write_end, false));
        ASSERT_TRUE(rig.console.set_std_handle(STD_OUTPUT_HANDLE, write_end));
        ASSERT_TRUE(rig.console.set_std_handle(STD_ERROR_HANDLE, write_end));

        // leaf: std out=pipe fd1_osf=pipe
        EXPECT_EQ(rig.call("_get_osfhandle", {1}), static_cast<u64>(write_end));
        EXPECT_EQ(rig.call("_get_osfhandle", {2}), static_cast<u64>(write_end));
        // leaf: isatty1=0 fileno_stdout=1 fstat1=0 mode=0x1000
        const u64 out_stream = rig.call("__acrt_iob_func", {1});
        ASSERT_NE(out_stream, 0u);
        EXPECT_EQ(static_cast<u32>(rig.call("_isatty", {1})), 0u);
        EXPECT_EQ(static_cast<u32>(rig.call("_fileno", {out_stream})), 1u);
        constexpr GuestAddr kStat = kBufAddr + 0x400;
        EXPECT_EQ(static_cast<u32>(rig.call("_fstat64", {1, kStat})), 0u);
        EXPECT_EQ(rig.memory.read_u16(kStat + 6).value_or(0) & 0xF000u, 0x1000u);

        // leaf: _write(1)\r\n, "_write returned 16": text mode, LF -> CR-LF,
        // and the count is the caller's bytes.
        ASSERT_TRUE(rig.m.write_ansi_string(kBufAddr, "leaf: _write(1)\n").is_ok());
        EXPECT_EQ(static_cast<u32>(rig.call("_write", {1, kBufAddr, 16})), 16u);
        EXPECT_EQ(rig.drain(read_end), "leaf: _write(1)\r\n");

        // {"leaf":"printf"}\r\n through a stream on stdout.
        ASSERT_TRUE(rig.m.write_ansi_string(kBufAddr, "{\"leaf\":\"fputs\"}\n").is_ok());
        EXPECT_NE(static_cast<u32>(rig.call("fputs", {kBufAddr, out_stream})), ~u32{0});
        EXPECT_EQ(rig.drain(read_end), "{\"leaf\":\"fputs\"}\r\n");

        // _setmode(1, _O_BINARY) returns the previous mode and stops the
        // translation.
        EXPECT_EQ(static_cast<u32>(rig.call("_setmode", {1, kO_BINARY})),
                  static_cast<u32>(kO_TEXT));
        ASSERT_TRUE(rig.m.write_ansi_string(kBufAddr, "raw\n").is_ok());
        EXPECT_EQ(static_cast<u32>(rig.call("_write", {1, kBufAddr, 4})), 4u);
        EXPECT_EQ(rig.drain(read_end), "raw\n");

        // Closing the descriptor closes its handle: the reader sees the end.
        EXPECT_EQ(static_cast<u32>(rig.call("_close", {1})), 0u);
        EXPECT_EQ(rig.handles.get_type(write_end), HandleType::Invalid);
        (void)rig.files.close_file(read_end);
    }
}

TEST_F(CrtStdioTest, StandardDescriptorsReadThroughAnInheritedPipe) {
    const std::filesystem::path dir = std::filesystem::path(tmp_path("stdpipein")).parent_path();
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        StdHandleRig rig(x64, dir);
        Handle read_end = 0;
        Handle write_end = 0;
        ASSERT_TRUE(rig.files.create_anonymous_pipe(read_end, write_end, false));
        ASSERT_TRUE(rig.console.set_std_handle(STD_INPUT_HANDLE, read_end));
        EXPECT_EQ(rig.call("_get_osfhandle", {0}), static_cast<u64>(read_end));
        // Learn _read: text mode collapses CR-LF to LF and keeps a lone CR.
        ASSERT_TRUE(rig.files.write_handle_bytes(write_end, "a\r\nb\rc"));
        ASSERT_TRUE(rig.files.close_file(write_end));
        EXPECT_EQ(static_cast<u32>(rig.call("_read", {0, kBufAddr, 64})), 5u);
        EXPECT_EQ(rig.text(kBufAddr, 5), "a\nb\rc");
        // The writer is gone: end of input.
        EXPECT_EQ(static_cast<u32>(rig.call("_read", {0, kBufAddr, 64})), 0u);
        (void)rig.files.close_file(read_end);
    }
}

TEST_F(CrtStdioTest, SeparateCrtDllArenasKeepIndependentModesAndDescriptorLifetimes) {
    // Native crt_read_validation_probe: closing msvcrt stdin leaves UCRT's
    // descriptor allocated. CRT global state and _setmode are DLL-local.
    const auto path = std::filesystem::path(tmp_path("crt_arenas"));
    { std::ofstream(path, std::ios::binary) << "AB"; }
    for (bool x64 : {false, true}) {
        StdHandleRig rig(x64, path.parent_path(), true);
        auto other = create_crt_stdio_thunks(rig.mgr, rig.vfs, rig.mm, rig.data,
                                            crt_stdio_buffer::Layout::UcrtOpaque32);
        const auto other_call = [&](const char* name, std::initializer_list<u64> args) {
            std::swap(rig.exports, other);
            const auto result = rig.call(name, args);
            std::swap(rig.exports, other);
            return result;
        };
        rig.mm.exchange_invalid_parameter_handler(0x12800);
        rig.mm.set_guest_caller([](GuestAddr, const u64*, usize) -> u64 { return 0; });
        ASSERT_TRUE(rig.m.write_ansi_string(kPathAddr, "C:\\" + path.filename().string()).is_ok());
        const auto fd = rig.call("_open", {kPathAddr, kO_BINARY, 0});
        const auto other_fd = other_call("_open", {kPathAddr, kO_BINARY, 0});
        EXPECT_EQ(fd, other_fd);
        ASSERT_NE(static_cast<u32>(fd), ~u32{0});
        EXPECT_EQ(rig.call("_read", {fd, kBufAddr, 1}), 1u);
        EXPECT_EQ(rig.text(kBufAddr, 1), "A");
        EXPECT_EQ(other_call("_read", {other_fd, kBufAddr, 1}), 1u);
        EXPECT_EQ(rig.text(kBufAddr, 1), "A");
        EXPECT_EQ(rig.call("_close", {fd}), 0u);
        EXPECT_EQ(other_call("_read", {other_fd, kBufAddr, 1}), 1u);
        EXPECT_EQ(rig.text(kBufAddr, 1), "B");
        EXPECT_EQ(other_call("_close", {other_fd}), 0u);
        EXPECT_EQ(rig.call("_setmode", {1, kO_BINARY}), 0x4000u);
        EXPECT_EQ(other_call("_setmode", {1, kO_BINARY}), 0x4000u);
        EXPECT_EQ(rig.call("_read", {0, 0, 0}), 0u);
        EXPECT_EQ(other_call("_read", {0, 0, 0}), 0u);
        EXPECT_EQ(rig.call("_close", {0}), 0u);
        EXPECT_EQ(other_call("_read", {0, 0, 0}), 0u);
        EXPECT_EQ(static_cast<u32>(rig.call("_read", {0, 0, 0})), ~u32{0});
        // Both CRTs initially referenced the same OS input handle. The second
        // close reports that OS failure and retires its own descriptor.
        EXPECT_EQ(static_cast<u32>(other_call("_close", {0})), ~u32{0});
    }
    std::filesystem::remove(path);
}

TEST_F(CrtStdioTest, CharacterCallbacksKeepTheirDllArenaAndBufferLayoutAfterRegistration) {
    const auto path = std::filesystem::path(tmp_path("character_arenas"));
    { std::ofstream(path, std::ios::binary) << "AB"; }
    for (bool x64 : {false, true}) {
        StdHandleRig rig(x64, path.parent_path(), true);
        auto other = create_crt_stdio_thunks(rig.mgr, rig.vfs, rig.mm, rig.data,
                                            crt_stdio_buffer::Layout::UcrtOpaque32);
        const auto other_call = [&](const char* name, std::initializer_list<u64> args) {
            std::swap(rig.exports, other);
            const auto result = rig.call(name, args);
            std::swap(rig.exports, other);
            return result;
        };
        ASSERT_TRUE(rig.m.write_ansi_string(kPathAddr, "C:\\" + path.filename().string()).is_ok());
        ASSERT_TRUE(rig.m.write_ansi_string(kModeAddr, "rb").is_ok());
        const auto legacy = rig.call("fopen", {kPathAddr, kModeAddr});
        const auto ucrt = other_call("fopen", {kPathAddr, kModeAddr});
        ASSERT_NE(legacy, 0u);
        ASSERT_NE(ucrt, 0u);
        EXPECT_EQ(rig.call("fgetc", {legacy}), 'A');
        EXPECT_EQ(other_call("fgetc", {ucrt}), 'A');
        EXPECT_EQ(rig.call("ungetc", {'Q', legacy}), 'Q');
        EXPECT_EQ(other_call("ungetc", {'R', ucrt}), 'R');
        EXPECT_EQ(rig.call("fgetc", {legacy}), 'Q');
        EXPECT_EQ(other_call("fgetc", {ucrt}), 'R');
        EXPECT_EQ(rig.call("fgetc", {legacy}), 'B');
        EXPECT_EQ(other_call("fgetc", {ucrt}), 'B');
        EXPECT_EQ(rig.call("fclose", {legacy}), 0u);
        EXPECT_EQ(other_call("fgetc", {ucrt}), ~u32{0});
        EXPECT_EQ(other_call("fclose", {ucrt}), 0u);
    }
    std::filesystem::remove(path);
}
