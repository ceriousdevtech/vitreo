/// @file test_module_thread_callbacks.cpp
/// @brief Tests for ModuleManager DLL_THREAD_ATTACH/DETACH dispatch to DLL and EXE TLS callbacks

#include "module_test_fixture.hpp"

TEST_P(DllNotificationDispatchTest,
       FinalUnloadStopsThreadAttachCallbacksUntilReload) {
    const bool is_64bit = GetParam();
    char root_template[] = "/tmp/vitreo_dll_thread_unload_XXXXXX";
    const char* root = ::mkdtemp(root_template);
    ASSERT_NE(root, nullptr);
    const std::filesystem::path root_path(root);
    const auto dll_bytes = build_static_tls_test_dll(is_64bit);
    {
        std::ofstream output(root_path / "tlsprobe.dll", std::ios::binary);
        output.write(reinterpret_cast<const char*>(dll_bytes.data()),
                     static_cast<std::streamsize>(dll_bytes.size()));
    }

    GuestMemory memory;
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
    ThunkRegistry registry(
        is_64bit ? 0x00007FFB00000000ull : 0x70000000ull);
    ModuleManager modules(registry);
    modules.attach_dynamic_loader(memory, vfs, is_64bit);
    std::vector<GuestAddr> thread_callbacks;
    std::vector<GuestAddr> thread_detach_callbacks;
    modules.set_guest_call(
        [&](GuestAddr callback, const u64* args, usize count,
            bool callback_is_64bit) -> u64 {
            EXPECT_EQ(count, 3u);
            EXPECT_EQ(callback_is_64bit, is_64bit);
            if (args[1] == 2) thread_callbacks.push_back(callback);
            if (args[1] == 3) thread_detach_callbacks.push_back(callback);
            return 1;
        });

    const auto install_tls_callback = [&](GuestAddr base) {
        if (is_64bit) {
            ASSERT_TRUE(memory.write_u64(base + 0x1118, base + 0x11A0).is_ok());
            ASSERT_TRUE(memory.write_u64(base + 0x11A0, base + 0x1010).is_ok());
            ASSERT_TRUE(memory.write_u64(base + 0x11A8, 0).is_ok());
        } else {
            ASSERT_TRUE(memory.write_u32(
                base + 0x110C, static_cast<u32>(base + 0x11A0)).is_ok());
            ASSERT_TRUE(memory.write_u32(
                base + 0x11A0, static_cast<u32>(base + 0x1010)).is_ok());
            ASSERT_TRUE(memory.write_u32(base + 0x11A4, 0).is_ok());
        }
    };
    const GuestAddr module = modules.load_library("C:\\tlsprobe.dll");
    ASSERT_NE(module, 0u);
    install_tls_callback(module);
    modules.notify_thread_attach();
    EXPECT_EQ(thread_callbacks,
              (std::vector<GuestAddr>{module + 0x1010, module + 0x1000}));
    modules.notify_thread_detach();
    EXPECT_EQ(thread_detach_callbacks,
              (std::vector<GuestAddr>{module + 0x1010, module + 0x1000}));

    ASSERT_TRUE(modules.free_library(module));
    thread_callbacks.clear();
    modules.notify_thread_attach();
    EXPECT_TRUE(thread_callbacks.empty());

    const GuestAddr reloaded = modules.load_library("C:\\tlsprobe.dll");
    ASSERT_NE(reloaded, 0u);
    install_tls_callback(reloaded);
    thread_callbacks.clear();
    modules.notify_thread_attach();
    EXPECT_EQ(thread_callbacks,
              (std::vector<GuestAddr>{reloaded + 0x1010, reloaded + 0x1000}));
    std::filesystem::remove_all(root_path);
}

// PE format "TLS Callback Functions": the EXE's TLS callbacks get
// DLL_THREAD_ATTACH / DLL_THREAD_DETACH for every thread but the first. Order
// measured by tools/oracle_probe/thread_lifecycle_probe.expected
// ("NOTIFY ... order=aABdD": DLL entry, EXE callback, body, DLL entry, EXE
// callback) -- the EXE comes after the DLLs in both directions.
TEST_P(DllNotificationDispatchTest, ExeTlsCallbacksFollowDllThreadNotifications) {
    const bool is_64bit = GetParam();
    char root_template[] = "/tmp/vitreo_exe_tls_notify_XXXXXX";
    const char* root = ::mkdtemp(root_template);
    ASSERT_NE(root, nullptr);
    const std::filesystem::path root_path(root);
    const auto dll_bytes = build_static_tls_test_dll(is_64bit);
    {
        std::ofstream output(root_path / "tlsprobe.dll", std::ios::binary);
        output.write(reinterpret_cast<const char*>(dll_bytes.data()),
                     static_cast<std::streamsize>(dll_bytes.size()));
    }
    GuestMemory memory;
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
    ThunkRegistry registry(is_64bit ? 0x00007FFB00000000ull : 0x70000000ull);
    ModuleManager modules(registry);
    modules.attach_dynamic_loader(memory, vfs, is_64bit);
    std::vector<std::pair<GuestAddr, u64>> calls;
    modules.set_guest_call([&](GuestAddr callback, const u64* args, usize,
                               bool) -> u64 {
        calls.emplace_back(callback, args[1]);
        return 1;
    });

    // A fake EXE image with a TLS directory at +0x100 whose callback array
    // (at +0x180) names one callback at +0x40.
    const GuestAddr exe = 0x00400000;
    ASSERT_TRUE(memory.map_region(exe, 0x1000, MemoryProtection::ReadWrite).is_ok());
    if (is_64bit) {
        ASSERT_TRUE(memory.write_u64(exe + 0x100 + 24, exe + 0x180).is_ok());
        ASSERT_TRUE(memory.write_u64(exe + 0x180, exe + 0x40).is_ok());
        ASSERT_TRUE(memory.write_u64(exe + 0x188, 0).is_ok());
    } else {
        ASSERT_TRUE(memory.write_u32(exe + 0x100 + 12, static_cast<u32>(exe + 0x180)).is_ok());
        ASSERT_TRUE(memory.write_u32(exe + 0x180, static_cast<u32>(exe + 0x40)).is_ok());
        ASSERT_TRUE(memory.write_u32(exe + 0x184, 0).is_ok());
    }
    modules.set_main_module(exe, "C:\\probe.exe");
    modules.set_main_module_tls(0x100, is_64bit ? 64u : 32u);

    const GuestAddr dll = modules.load_library("C:\\tlsprobe.dll");
    ASSERT_NE(dll, 0u);
    calls.clear();
    modules.notify_thread_attach();
    ASSERT_FALSE(calls.empty());
    EXPECT_EQ(calls.back(), (std::pair<GuestAddr, u64>{exe + 0x40, 2}));
    for (usize i = 0; i + 1 < calls.size(); ++i) EXPECT_NE(calls[i].first, exe + 0x40);
    calls.clear();
    modules.notify_thread_detach();
    ASSERT_FALSE(calls.empty());
    EXPECT_EQ(calls.back(), (std::pair<GuestAddr, u64>{exe + 0x40, 3}));
    for (usize i = 0; i + 1 < calls.size(); ++i) EXPECT_NE(calls[i].first, exe + 0x40);
    std::filesystem::remove_all(root_path);
}
