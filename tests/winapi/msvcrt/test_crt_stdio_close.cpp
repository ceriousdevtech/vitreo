// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crt_stdio_close.cpp
/// @brief _close of console, failing, stream-backed and protected-handle descriptors

#include "crt_stdio_test_fixture.hpp"

// Native Windows: crt_close_console_probe.expected, UCRT, both bitnesses.
TEST_F(CrtStdioTest, CloseConsoleDescriptorReleasesHandleAndReusesDescriptor) {
    const auto dir = std::filesystem::path(tmp_path("closeconsole")).parent_path();
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        StdHandleRig rig(x64, dir, true);
        unsigned invalid_calls = 0;
        rig.mm.exchange_invalid_parameter_handler(0x12800);
        rig.mm.set_guest_caller([&](GuestAddr, const u64*, usize) -> u64 {
            ++invalid_calls; return 0;
        });
        const Handle handle = rig.console.std_slot(1);
        ASSERT_EQ(rig.handles.get_type(handle), HandleType::ConsoleOutput);
        const u64 stream = rig.call("__acrt_iob_func", {1});
        EXPECT_EQ(rig.call("_get_osfhandle", {1}), handle);
        EXPECT_EQ(rig.call("_isatty", {1}), 64u);
        EXPECT_EQ(rig.call("_close", {1}), 0u);
        EXPECT_EQ(rig.handles.get_type(handle), HandleType::Invalid);
        EXPECT_EQ(rig.console.std_slot(1), NULL_HANDLE);
        EXPECT_EQ(rig.call("_fileno", {stream}), 1u);
        rig.mm.set_errno(0);
        EXPECT_TRUE(rig.is_invalid_handle(rig.call("_get_osfhandle", {1})));
        EXPECT_EQ(rig.mm.get_errno(), 9u);
        EXPECT_EQ(invalid_calls, 1u);
        rig.mm.set_errno(0);
        EXPECT_EQ(rig.call("_isatty", {1}), 0u);
        EXPECT_EQ(rig.mm.get_errno(), 0u);
        EXPECT_EQ(invalid_calls, 1u);
        EXPECT_EQ(static_cast<u32>(rig.call("_close", {1})), ~u32{0});
        EXPECT_EQ(rig.mm.get_errno(), 9u);
        EXPECT_EQ(invalid_calls, 2u);
        Handle read_end = 0, write_end = 0;
        ASSERT_TRUE(rig.files.create_anonymous_pipe(read_end, write_end, false));
        const u64 fd = rig.call("_open_osfhandle", {write_end, kO_BINARY});
        EXPECT_EQ(fd, 1u);
        EXPECT_EQ(rig.call("_get_osfhandle", {fd}), write_end);
        ASSERT_TRUE(rig.m.write_ansi_string(kBufAddr, "reused").is_ok());
        EXPECT_EQ(rig.call("_write", {fd, kBufAddr, 6}), 6u);
        EXPECT_EQ(rig.drain(read_end), "reused");
        EXPECT_EQ(rig.call("_close", {fd}), 0u);
        EXPECT_EQ(rig.handles.get_type(write_end), HandleType::Invalid);
        rig.mm.set_errno(0);
        EXPECT_EQ(static_cast<u32>(rig.call("fclose", {stream})), ~u32{0});
        EXPECT_EQ(rig.mm.get_errno(), 9u);
        EXPECT_EQ(invalid_calls, 3u);
        // The guest stream never owns the runtime's host logging descriptor.
        EXPECT_NE(::fcntl(STDOUT_FILENO, F_GETFD), -1);
        (void)rig.files.close_file(read_end);
    }
}

TEST_F(CrtStdioTest, CloseReportsUnderlyingHandleFailureAndReleasesDescriptor) {
    const auto dir = std::filesystem::path(tmp_path("closeinvalidos")).parent_path();
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        OsHandleRig rig(x64, dir);
        unsigned invalid_calls = 0;
        rig.mm.exchange_invalid_parameter_handler(0x12800);
        rig.mm.set_guest_caller([&](GuestAddr, const u64*, usize) -> u64 {
            ++invalid_calls; return 0;
        });
        Handle read_end = 0, write_end = 0;
        ASSERT_TRUE(rig.files.create_anonymous_pipe(read_end, write_end, false));
        const u64 fd = rig.call("_open_osfhandle", {write_end, kO_BINARY});
        ASSERT_TRUE(rig.files.close_file(write_end));
        rig.mm.set_errno(0);
        EXPECT_EQ(static_cast<u32>(rig.call("_close", {fd})), ~u32{0});
        EXPECT_EQ(rig.mm.get_errno(), 9u);
        EXPECT_EQ(invalid_calls, 0u);
        EXPECT_TRUE(rig.is_invalid_handle(rig.call("_get_osfhandle", {fd})));
        EXPECT_EQ(invalid_calls, 1u);
        EXPECT_EQ(static_cast<u32>(rig.call("_close", {~u32{0}})), ~u32{0});
        EXPECT_EQ(invalid_calls, 2u);
        (void)rig.files.close_file(read_end);
    }
}

TEST_F(CrtStdioTest, CloseStreamDescriptorClosesTheSameHandle) {
    const auto path = std::filesystem::path(tmp_path("closealias"));
    for (bool x64 : {false, true}) {
        for (auto flavor : {CrtDllFlavor::Ucrt, CrtDllFlavor::Msvcrt}) {
            SCOPED_TRACE(x64 ? "x64" : "x86");
            CrtDllFlavorScope scope(flavor);
            StdHandleRig rig(x64, path.parent_path(), true);
            unsigned invalid_calls = 0;
            // The native legacy DLL has no invalid-handler setter export.
            if (flavor == CrtDllFlavor::Ucrt) {
                rig.mm.exchange_invalid_parameter_handler(0x12800);
                rig.mm.set_guest_caller([&](GuestAddr, const u64*, usize) -> u64 {
                    ++invalid_calls; return 0;
                });
            }
            EXPECT_EQ(rig.call("_close", {1}), 0u);
            ASSERT_TRUE(rig.m.write_ansi_string(kPathAddr, "C:\\" + path.filename().string()).is_ok());
            ASSERT_TRUE(rig.m.write_ansi_string(kBufAddr, "wb").is_ok());
            const auto stream = rig.call("fopen", {kPathAddr, kBufAddr});
            ASSERT_NE(stream, 0u);
            const auto fd = rig.call("_fileno", {stream});
            EXPECT_EQ(fd, 1u);
            const auto handle = static_cast<Handle>(rig.call("_get_osfhandle", {fd}));
            EXPECT_EQ(rig.handles.get_type(handle), HandleType::File);
            EXPECT_EQ(rig.call("_close", {fd}), 0u);
            EXPECT_EQ(rig.handles.get_type(handle), HandleType::Invalid);
            rig.mm.set_errno(0);
            EXPECT_EQ(static_cast<u32>(rig.call("fclose", {stream})), ~u32{0});
            EXPECT_EQ(rig.mm.get_errno(), 9u);
            EXPECT_EQ(invalid_calls, flavor == CrtDllFlavor::Ucrt ? 1u : 0u);
            invalid_calls = 0;
            const auto stream2 = rig.call("fopen", {kPathAddr, kBufAddr});
            ASSERT_NE(stream2, 0u);
            const auto fd2 = rig.call("_fileno", {stream2});
            const auto handle2 = static_cast<Handle>(rig.call("_get_osfhandle", {fd2}));
            ASSERT_TRUE(rig.files.close_file(handle2));
            rig.mm.set_errno(0);
            EXPECT_EQ(static_cast<u32>(rig.call("fclose", {stream2})), ~u32{0});
            EXPECT_EQ(rig.mm.get_errno(), 9u);
            EXPECT_EQ(invalid_calls, 0u);
        }
    }
    std::filesystem::remove(path);
}

TEST_F(CrtStdioTest, CloseProtectedHandleRetiresDescriptorButKeepsOSHandle) {
    const auto dir = std::filesystem::path(tmp_path("closeprotected")).parent_path();
    for (bool x64 : {false, true}) {
        OsHandleRig rig(x64, dir);
        unsigned invalid_calls = 0;
        rig.mm.exchange_invalid_parameter_handler(0x12800);
        rig.mm.set_guest_caller([&](GuestAddr, const u64*, usize) -> u64 {
            ++invalid_calls; return 0;
        });
        Handle read_end = 0, write_end = 0;
        ASSERT_TRUE(rig.files.create_anonymous_pipe(read_end, write_end, false));
        const auto fd = rig.call("_open_osfhandle", {write_end, kO_BINARY});
        ASSERT_TRUE(rig.handles.set_flags(write_end, HandleFlags::ProtectFromClose, HandleFlags::ProtectFromClose));
        rig.mm.set_errno(0);
        EXPECT_EQ(static_cast<u32>(rig.call("_close", {fd})), ~u32{0});
        EXPECT_EQ(rig.mm.get_errno(), 9u);
        EXPECT_EQ(invalid_calls, 0u);
        EXPECT_EQ(rig.handles.get_type(write_end), HandleType::Pipe);
        EXPECT_TRUE(rig.is_invalid_handle(rig.call("_get_osfhandle", {fd})));
        EXPECT_EQ(invalid_calls, 1u);
        ASSERT_TRUE(rig.handles.set_flags(write_end, HandleFlags::ProtectFromClose, 0));
        EXPECT_TRUE(rig.files.close_file(write_end));
        EXPECT_TRUE(rig.files.close_file(read_end));
    }
}
