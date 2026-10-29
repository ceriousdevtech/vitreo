// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crt_stdio_osfhandle.cpp
/// @brief _read validation precedence, _get_osfhandle and _open_osfhandle

#include "crt_stdio_test_fixture.hpp"

TEST_F(CrtStdioTest, ReadValidationPrecedenceMatchesWindowsBothFamiliesAndBitnesses) {
    // crt_read_validation_probe.expected: descriptor validity precedes both
    // zero-length reads and buffer/count validation; -2 skips the handler.
    const std::filesystem::path path = tmp_path("read_validation");
    { std::ofstream(path, std::ios::binary) << "A"; }
    for (bool x64 : {false, true}) {
        for (auto flavor : {CrtDllFlavor::Ucrt, CrtDllFlavor::Msvcrt}) {
            SCOPED_TRACE(x64 ? "x64" : "x86");
            CrtDllFlavorScope scope(flavor);
            OsHandleRig rig(x64, path.parent_path());
            ConsoleManager console(rig.handles, true);
            rig.files.set_console_manager(console);
            rig.mgr.set_console_manager(&console);
            unsigned invalid_calls = 0;
            if (flavor == CrtDllFlavor::Ucrt) {
                rig.mm.exchange_invalid_parameter_handler(0x12800);
                rig.mm.set_guest_caller([&](GuestAddr, const u64*, usize) -> u64 {
                    ++invalid_calls;
                    return 0;
                });
            }
            ASSERT_TRUE(rig.m.write_ansi_string(
                kPathAddr, "C:\\" + path.filename().string()).is_ok());
            const auto fd = rig.call("_open", {kPathAddr, kO_BINARY, 0});
            ASSERT_NE(static_cast<u32>(fd), ~u32{0});
            auto check = [&](i32 descriptor, u32 count, bool null_buffer,
                             u32 expected, u32 error, unsigned handler_count) {
                ASSERT_TRUE(rig.memory.write_u32(kBufAddr, 0xcccccccc).is_ok());
                rig.mm.set_errno(123);
                invalid_calls = 0;
                EXPECT_EQ(static_cast<u32>(rig.call("_read", {
                    static_cast<u32>(descriptor), null_buffer ? 0 : kBufAddr, count})), expected);
                EXPECT_EQ(rig.mm.get_errno(), error);
                EXPECT_EQ(invalid_calls, handler_count);
                EXPECT_EQ(rig.memory.read_u32(kBufAddr).value_or(0), 0xccccccccu);
            };
            const unsigned handlers = flavor == CrtDllFlavor::Ucrt ? 1u : 0u;
            for (i32 bad_fd : {-1, -2, -99, 999999}) {
                for (u32 count : {0u, 1u, 0x80000000u}) {
                    for (bool null_buffer : {false, true})
                        check(bad_fd, count, null_buffer, ~u32{0}, 9,
                              bad_fd == -2 ? 0u : handlers);
                }
            }
            for (bool null_buffer : {false, true})
                check(static_cast<i32>(fd), 0, null_buffer, 0, 123, 0);
            check(static_cast<i32>(fd), 1, true, ~u32{0}, 22, handlers);
            for (bool null_buffer : {false, true})
                check(static_cast<i32>(fd), 0x80000000u, null_buffer, ~u32{0}, 22, handlers);
            EXPECT_EQ(rig.call("_close", {fd}), 0u);
            for (u32 count : {0u, 1u, 0x80000000u})
                for (bool null_buffer : {false, true})
                    check(static_cast<i32>(fd), count, null_buffer, ~u32{0}, 9, handlers);

            // A standard descriptor changes validity when retired and reused.
            check(0, 0, true, 0, 123, 0);
            EXPECT_EQ(rig.call("_close", {0}), 0u);
            for (u32 count : {0u, 1u, 0x80000000u})
                for (bool null_buffer : {false, true})
                    check(0, count, null_buffer, ~u32{0}, 9, handlers);
            const auto reused_fd = rig.call("_open", {kPathAddr, kO_BINARY, 0});
            EXPECT_EQ(reused_fd, 0u);
            check(0, 0, true, 0, 123, 0);
            EXPECT_EQ(rig.call("_close", {0}), 0u);
        }
    }
    std::filesystem::remove(path);
}

TEST_F(CrtStdioTest, GetOsfhandleReturnsTheKernelHandleBehindADescriptor) {
    const std::filesystem::path path = tmp_path("osfhandle");
    { std::ofstream(path, std::ios::binary) << "hello world"; }
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        OsHandleRig rig(x64, path.parent_path());
        ASSERT_TRUE(rig.m.write_ansi_string(
            kPathAddr, "C:\\" + path.filename().string()).is_ok());
        const u64 fd = rig.call("_open", {kPathAddr, kO_RDONLY | kO_BINARY, 0});
        ASSERT_NE(static_cast<u32>(fd), ~u32{0});

        const u64 h = rig.call("_get_osfhandle", {fd});
        ASSERT_FALSE(rig.is_invalid_handle(h));
        EXPECT_EQ(rig.handles.get_type(static_cast<Handle>(h)), HandleType::File);
        EXPECT_EQ(rig.call("_get_osfhandle", {fd}), h) << "stable across calls";

        // One file object: a _read and a ReadFile advance the same position.
        EXPECT_EQ(rig.call("_read", {fd, kBufAddr, 3}), 3u);
        EXPECT_EQ(rig.text(kBufAddr, 3), "hel");
        u32 got = 0;
        ASSERT_TRUE(rig.files.read_file(static_cast<Handle>(h), kBufAddr + 0x100,
                                        3, 0, &got));
        EXPECT_EQ(got, 3u);
        EXPECT_EQ(rig.text(kBufAddr + 0x100, 3), "lo ");

        // _close releases the handle it owns.
        EXPECT_EQ(static_cast<u32>(rig.call("_close", {fd})), 0u);
        EXPECT_EQ(rig.handles.get_type(static_cast<Handle>(h)),
                  HandleType::Invalid);

        // Invalid fd: handler once, then INVALID_HANDLE_VALUE and EBADF.
        unsigned invalid_calls = 0;
        rig.mm.exchange_invalid_parameter_handler(0x12800);
        rig.mm.set_guest_caller([&](GuestAddr, const u64*, usize) -> u64 {
            ++invalid_calls;
            return 0;
        });
        rig.mm.set_errno(0);
        EXPECT_TRUE(rig.is_invalid_handle(rig.call("_get_osfhandle", {fd})));
        EXPECT_EQ(rig.mm.get_errno(), 9u);
        EXPECT_EQ(invalid_calls, 1u);
    }
    std::filesystem::remove(path);
}

TEST_F(CrtStdioTest, OpenOsfhandleTakesOwnershipOfFileAndPipeHandles) {
    const std::filesystem::path path = tmp_path("openosf");
    { std::ofstream(path, std::ios::binary) << "hello world"; }
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        OsHandleRig rig(x64, path.parent_path());
        const Handle file = rig.files.create_file(
            "C:\\" + path.filename().string(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, 0);
        ASSERT_NE(file, INVALID_HANDLE);
        const u64 fd = rig.call("_open_osfhandle", {file, kO_RDONLY});
        ASSERT_NE(static_cast<u32>(fd), ~u32{0});
        EXPECT_EQ(rig.call("_get_osfhandle", {fd}), static_cast<u64>(file));
        EXPECT_EQ(rig.call("_read", {fd, kBufAddr, 5}), 5u);
        EXPECT_EQ(rig.text(kBufAddr, 5), "hello");
        EXPECT_EQ(static_cast<u32>(rig.call("_close", {fd})), 0u);
        EXPECT_EQ(rig.handles.get_type(file), HandleType::Invalid)
            << "_close closes the handle _open_osfhandle took over";

        // A pipe has no file behind it: the descriptors read and write
        // through the handles.
        Handle read_end = 0;
        Handle write_end = 0;
        ASSERT_TRUE(rig.files.create_anonymous_pipe(read_end, write_end, false));
        const u64 wfd = rig.call("_open_osfhandle", {write_end, 0});
        const u64 rfd = rig.call("_open_osfhandle", {read_end, kO_RDONLY});
        ASSERT_NE(static_cast<u32>(wfd), ~u32{0});
        ASSERT_NE(static_cast<u32>(rfd), ~u32{0});
        ASSERT_TRUE(rig.m.write_ansi_string(kBufAddr, "abc").is_ok());
        EXPECT_EQ(rig.call("_write", {wfd, kBufAddr, 3}), 3u);
        EXPECT_EQ(rig.call("_read", {rfd, kBufAddr + 0x100, 3}), 3u);
        EXPECT_EQ(rig.text(kBufAddr + 0x100, 3), "abc");
        EXPECT_EQ(static_cast<u32>(rig.call("_close", {wfd})), 0u);
        EXPECT_EQ(static_cast<u32>(rig.call("_close", {rfd})), 0u);
        EXPECT_EQ(rig.handles.get_type(read_end), HandleType::Invalid);
        EXPECT_EQ(rig.handles.get_type(write_end), HandleType::Invalid);

        // Not a handle: -1 and EBADF.
        rig.mm.set_errno(0);
        EXPECT_EQ(static_cast<u32>(rig.call("_open_osfhandle", {0x7654321, 0})),
                  ~u32{0});
        EXPECT_EQ(rig.mm.get_errno(), 9u);
    }
    std::filesystem::remove(path);
}
