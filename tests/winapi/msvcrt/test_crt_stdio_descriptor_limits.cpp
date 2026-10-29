// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crt_stdio_descriptor_limits.cpp
/// @brief _dup/_dup2 validation, no-console close and full descriptor arenas

#include "crt_stdio_test_fixture.hpp"

TEST_F(CrtStdioTest, DescriptorDuplicationMatchesNativeValidationOwnershipAndLimits) {
    const auto path = std::filesystem::path(tmp_path("crt_dup"));
    { std::ofstream(path, std::ios::binary) << "abcdef"; }
    for (bool x64 : {false, true}) {
        for (auto flavor : {CrtDllFlavor::Ucrt, CrtDllFlavor::Msvcrt}) {
            CrtDllFlavorScope scope(flavor);
            StdHandleRig rig(x64, path.parent_path(), true);
            unsigned invalid_calls = 0;
            if (flavor == CrtDllFlavor::Ucrt) {
                rig.mm.exchange_invalid_parameter_handler(0x12800);
                rig.mm.set_guest_caller([&](GuestAddr, const u64*, usize) -> u64 {
                    ++invalid_calls; return 0;
                });
            }
            for (i32 source : {-2, -1}) {
                for (i32 target : {-2, -1}) {
                    rig.mm.set_errno(123); invalid_calls = 0;
                    EXPECT_EQ(static_cast<u32>(rig.call("_dup2", {
                        static_cast<u32>(source), static_cast<u32>(target)})), ~u32{0});
                    EXPECT_EQ(rig.mm.get_errno(), 9u);
                    EXPECT_EQ(invalid_calls, source == -1 && flavor == CrtDllFlavor::Ucrt ? 1u : 0u);
                }
            }
            ASSERT_TRUE(rig.m.write_ansi_string(kPathAddr, "C:\\" + path.filename().string()).is_ok());
            const auto fd = rig.call("_open", {kPathAddr, kO_BINARY, 0});
            const auto source_handle = static_cast<Handle>(rig.call("_get_osfhandle", {fd}));
            for (i32 target : {-2, -1, 0, 5, 2047, 2048, 8191, 8192, 0x7fffffff}) {
                const bool allowed = target >= 0 && target <
                    (flavor == CrtDllFlavor::Msvcrt ? 2048 : 8192);
                rig.mm.set_errno(123); invalid_calls = 0;
                EXPECT_EQ(static_cast<u32>(rig.call("_dup2", {fd, static_cast<u32>(target)})),
                          allowed ? 0u : ~u32{0});
                EXPECT_EQ(rig.mm.get_errno(), allowed ? 123u : 9u);
                EXPECT_EQ(invalid_calls, !allowed && target != -2 && flavor == CrtDllFlavor::Ucrt ? 1u : 0u);
                if (!allowed) continue;
                const auto copy_handle = static_cast<Handle>(rig.call("_get_osfhandle", {static_cast<u32>(target)}));
                EXPECT_NE(copy_handle, source_handle);
                EXPECT_NE(rig.handles.get_flags(copy_handle) & HandleFlags::Inherit, 0u);
                if (target == 0) EXPECT_EQ(rig.console.std_slot(0), copy_handle);
                EXPECT_EQ(rig.call("_close", {static_cast<u32>(target)}), 0u);
            }
            EXPECT_EQ(rig.call("_dup2", {fd, fd}), 0u);
            const auto copy = rig.call("_dup", {fd});
            EXPECT_EQ(copy, 0u) << "lowest free descriptor is standard input";
            EXPECT_EQ(rig.call("_read", {fd, kBufAddr, 1}), 1u);
            EXPECT_EQ(rig.text(kBufAddr, 1), "a");
            EXPECT_EQ(rig.call("_read", {copy, kBufAddr, 1}), 1u);
            EXPECT_EQ(rig.text(kBufAddr, 1), "b");
            EXPECT_EQ(rig.call("_close", {fd}), 0u);
            EXPECT_EQ(rig.call("_read", {copy, kBufAddr, 1}), 1u);
            EXPECT_EQ(rig.text(kBufAddr, 1), "c");
            const auto external = static_cast<Handle>(rig.call("_get_osfhandle", {copy}));
            ASSERT_TRUE(rig.files.close_file(external));
            rig.mm.set_errno(123); invalid_calls = 0;
            EXPECT_EQ(static_cast<u32>(rig.call("_dup", {copy})), ~u32{0});
            EXPECT_EQ(rig.mm.get_errno(), 9u);
            EXPECT_EQ(invalid_calls, 0u);
            rig.mm.set_errno(123);
            EXPECT_EQ(rig.call("_dup2", {copy, copy}), 0u);
            EXPECT_EQ(rig.mm.get_errno(), 123u);
            EXPECT_EQ(static_cast<u32>(rig.call("_close", {copy})), ~u32{0});
        }
    }
    std::filesystem::remove(path);
}

TEST_F(CrtStdioTest, NoConsoleDescriptorsCloseSuccessfullyAndDuplicateTheCurrentThreadHandle) {
    const auto dir = std::filesystem::path(tmp_path("crt_none")).parent_path();
    for (bool x64 : {false, true}) {
        for (auto flavor : {CrtDllFlavor::Msvcrt, CrtDllFlavor::Ucrt}) {
            CrtDllFlavorScope scope(flavor);
            StdHandleRig rig(x64, dir);
            ThreadManager threads(rig.handles);
            rig.mgr.set_thread_manager(&threads);
            unsigned invalid_calls = 0;
            if (flavor == CrtDllFlavor::Ucrt) {
                rig.mm.exchange_invalid_parameter_handler(0x12800);
                rig.mm.set_guest_caller([&](GuestAddr, const u64*, usize) -> u64 {
                    ++invalid_calls; return 0;
                });
            }
            const auto check = [&](const char* name, std::initializer_list<u64> args,
                                   u32 expected, u32 error, unsigned invalid = 0) {
                rig.mm.set_errno(123); invalid_calls = 0;
                EXPECT_EQ(static_cast<u32>(rig.call(name, args)), expected) << name;
                EXPECT_EQ(rig.mm.get_errno(), error) << name;
                EXPECT_EQ(invalid_calls, invalid) << name;
            };
            check("_get_osfhandle", {0}, static_cast<u32>(-2), 123);
            check("_read", {0, 0, 0}, 0, 123);
            check("_read", {0, kBufAddr, 1}, ~u32{0}, 9);
            check("_read", {0, 0, 1}, ~u32{0}, 22, flavor == CrtDllFlavor::Ucrt ? 1 : 0);
            check("_dup", {0}, 3, 123);
            const auto copied = static_cast<Handle>(rig.call("_get_osfhandle", {3}));
            ASSERT_EQ(rig.handles.get_type(copied), HandleType::Thread);
            EXPECT_EQ(threads.get_thread_by_handle(copied)->thread_id, threads.get_current_thread_id());
            EXPECT_NE(rig.handles.get_flags(copied) & HandleFlags::Inherit, 0u);
            check("_read", {3, 0, 0}, 0, 123);
            check("_read", {3, kBufAddr, 1}, ~u32{0}, 9);
            check("_dup2", {3, 3}, 0, 123);
            check("_close", {3}, 0, 123);
            check("_dup2", {0, 5}, 0, 123);
            const auto ordinary = static_cast<Handle>(rig.call("_get_osfhandle", {5}));
            EXPECT_EQ(rig.handles.get_type(ordinary), HandleType::Thread);
            check("_close", {5}, 0, 123);
            check("_dup2", {0, 1}, 0, 123);
            EXPECT_EQ(rig.handles.get_type(static_cast<Handle>(rig.call("_get_osfhandle", {1}))), HandleType::Thread);
            check("_dup2", {0, 0}, 0, 123);
            check("_close", {0}, 0, 123);
            check("_read", {0, 0, 0}, ~u32{0}, 9, flavor == CrtDllFlavor::Ucrt ? 1 : 0);
            check("_get_osfhandle", {0}, ~u32{0}, 9, flavor == CrtDllFlavor::Ucrt ? 1 : 0);
            check("_close", {1}, 0, 123);
        }
    }
}

TEST_F(CrtStdioTest, FullDescriptorArenasFailBeforeCreatingFilesAndKeepUnadoptedHandles) {
    // crt_dup_capacity_probe.expected: EMFILE, no invalid handler, no created
    // file, and _open_osfhandle failure leaves the HANDLE with its caller.
    const auto path = std::filesystem::path(tmp_path("crt_full"));
    for (bool x64 : {false, true}) {
        for (auto flavor : {CrtDllFlavor::Ucrt, CrtDllFlavor::Msvcrt}) {
            CrtDllFlavorScope scope(flavor);
            StdHandleRig rig(x64, path.parent_path(), true);
            unsigned invalid_calls = 0;
            if (flavor == CrtDllFlavor::Ucrt) {
                rig.mm.exchange_invalid_parameter_handler(0x12800);
                rig.mm.set_guest_caller([&](GuestAddr, const u64*, usize) -> u64 {
                    ++invalid_calls; return 0;
                });
            }
            Handle read, write;
            ASSERT_TRUE(rig.files.create_anonymous_pipe(read, write, false));
            const auto fd = rig.call("_open_osfhandle", {read, kO_BINARY});
            ASSERT_EQ(fd, 3u);
            const u32 limit = flavor == CrtDllFlavor::Msvcrt ? 2048u : 8192u;
            for (u32 target = 4; target < limit; ++target)
                ASSERT_EQ(rig.call("_dup2", {fd, target}), 0u);
            const auto fail = [&](const char* name, std::initializer_list<u64> args, u32 result) {
                rig.mm.set_errno(123); invalid_calls = 0;
                EXPECT_EQ(static_cast<u32>(rig.call(name, args)), result);
                EXPECT_EQ(rig.mm.get_errno(), 24u);
                EXPECT_EQ(invalid_calls, 0u);
            };
            fail("_dup", {fd}, ~u32{0});
            ASSERT_TRUE(rig.m.write_ansi_string(kPathAddr, "C:\\" + path.filename().string()).is_ok());
            fail("_open", {kPathAddr, kO_BINARY | 0x301u, 0x180u}, ~u32{0});
            EXPECT_FALSE(std::filesystem::exists(path));
            ASSERT_TRUE(rig.m.write_ansi_string(kModeAddr, "wb").is_ok());
            fail("fopen", {kPathAddr, kModeAddr}, 0);
            EXPECT_FALSE(std::filesystem::exists(path));
            Handle extra_read, extra_write;
            ASSERT_TRUE(rig.files.create_anonymous_pipe(extra_read, extra_write, false));
            fail("_open_osfhandle", {extra_read, kO_BINARY}, ~u32{0});
            EXPECT_EQ(rig.handles.get_type(extra_read), HandleType::Pipe);
            for (u32 allocated = 3; allocated < limit; ++allocated)
                EXPECT_EQ(rig.call("_close", {allocated}), 0u);
            EXPECT_TRUE(rig.files.close_file(write));
            EXPECT_TRUE(rig.files.close_file(extra_read));
            EXPECT_TRUE(rig.files.close_file(extra_write));
        }
    }
}
