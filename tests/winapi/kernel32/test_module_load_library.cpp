/// @file test_module_load_library.cpp
/// @brief Tests for ModuleManager LoadLibrary: known DLLs, path equivalence, search order, bitness, API sets

#include "module_test_fixture.hpp"

// =============================================================================
// LoadLibrary Tests
// =============================================================================

TEST_F(ModuleManagerTest, LoadLibraryKnownDll) {
    // ntdll is registered in the ThunkRegistry but not yet in the ModuleManager
    set_last_error(0xDEADu);
    GuestAddr base = mm_->load_library("ntdll.dll");
    EXPECT_NE(base, 0u);
    EXPECT_EQ(base, ModuleManager::NTDLL_BASE);
    EXPECT_EQ(get_last_error(), 0xDEADu);  // success leaves it untouched (last_error_on_success_probe.expected)
}

TEST_F(ModuleManagerTest, LoadLibraryAlreadyLoadedIncrementsRefCount) {
    GuestAddr first = mm_->get_module_handle("kernel32.dll");
    GuestAddr second = mm_->load_library("kernel32.dll");
    EXPECT_EQ(first, second);
}

TEST_F(ModuleManagerTest, LoadLibraryUnknownDllReturnsZero) {
    GuestAddr base = mm_->load_library("doesnotexist.dll");
    EXPECT_EQ(base, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_MOD_NOT_FOUND);
}

TEST_F(ModuleManagerTest, LoadLibraryCaseInsensitive) {
    GuestAddr base = mm_->load_library("NTDLL.DLL");
    EXPECT_NE(base, 0u);
}

TEST(ModuleManagerConcurrencyTest, ConcurrentEquivalentPathsMapOneModule) {
    char root_template[] = "/tmp/vitreo_dll_loader_lock_XXXXXX";
    const char* root = ::mkdtemp(root_template);
    ASSERT_NE(root, nullptr);
    const std::filesystem::path root_path(root);
    const auto dll_bytes = build_notification_test_dll(/*is_64bit=*/true);
    {
        std::ofstream output(root_path / "race.dll", std::ios::binary);
        output.write(reinterpret_cast<const char*>(dll_bytes.data()),
                     static_cast<std::streamsize>(dll_bytes.size()));
    }

    GuestMemory memory;
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
    ThunkRegistry registry(0x00007FFB00000000ull);
    ModuleManager modules(registry);
    modules.attach_dynamic_loader(memory, vfs, /*is_64bit_process=*/true);
    modules.set_main_module(0x0000000140000000ull, "C:\\test.exe");

    const GuestAddr implicit_base = modules.load_library("race.dll");
    ASSERT_NE(implicit_base, 0u);

    constexpr usize kThreads = 16;
    std::array<GuestAddr, kThreads> results{};
    std::atomic<usize> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (usize i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            results[i] = modules.load_library(
                (i & 1u) == 0 ? "C:\\race.dll" : "C:/race.dll");
        });
    }
    while (ready.load(std::memory_order_acquire) != kThreads) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();

    ASSERT_EQ(results[0], implicit_base);
    for (const GuestAddr result : results) {
        EXPECT_EQ(result, results[0]);
    }
    EXPECT_EQ(modules.module_count(), 1u);

    std::filesystem::remove_all(root_path);
}

// A module is named by its full path: "..", ".", "/", doubled separators, a
// missing directory undone by "..", trailing dots/spaces and case all reach
// the module of the canonical spelling, and a module first loaded through such
// a spelling reports the canonical path. Identical on x86 and x64
// (tools/oracle_probe/loader_path_canon_probe.c,
// tests/oracle_data/loader_path_canon_probe{32,64}.expected).
TEST(ModuleManagerPathIdentityTest, NonCanonicalPathsNameTheLoadedModule) {
    for (const bool is_64bit : {false, true}) {
        SCOPED_TRACE(is_64bit ? "x64" : "x86");
        char root_template[] = "/tmp/vitreo_dll_path_canon_XXXXXX";
        const char* root = ::mkdtemp(root_template);
        ASSERT_NE(root, nullptr);
        const std::filesystem::path root_path(root);
        std::filesystem::create_directories(root_path / "program");
        const auto dll_bytes = build_notification_test_dll(is_64bit);
        for (const char* name : {"canon.dll", "fresh_dot.dll", "fresh_missing.dll", "fresh_slash.dll",
                                 "fresh_dsep.dll", "fresh_tdot.dll"}) {
            std::ofstream output(root_path / "program" / name, std::ios::binary);
            output.write(reinterpret_cast<const char*>(dll_bytes.data()),
                         static_cast<std::streamsize>(dll_bytes.size()));
        }

        GuestMemory memory;
        capsule::Vfs vfs;
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
        ThunkRegistry registry(0x00007FFB00000000ull);
        ModuleManager modules(registry);
        modules.attach_dynamic_loader(memory, vfs, is_64bit);
        modules.set_main_module(is_64bit ? 0x0000000140000000ull : 0x00400000ull, "C:\\program\\test.exe");

        // "== canonical first": every odd spelling is the same module.
        const GuestAddr canon = modules.load_library("C:\\program\\canon.dll");
        ASSERT_NE(canon, 0u);
        for (const char* odd : {"C:\\program\\..\\program\\canon.dll", "C:\\program\\.\\canon.dll",
                                "C:\\program/canon.dll", "C:\\program\\\\canon.dll",
                                "C:\\program\\nonexistent_dir\\..\\canon.dll", "C:\\program\\canon.dll.",
                                "C:\\program\\canon.dll ", "C:\\PROGRAM\\CANON.DLL"}) {
            SCOPED_TRACE(odd);
            EXPECT_EQ(modules.get_module_handle(odd), canon);
            EXPECT_EQ(modules.load_library(odd), canon);
        }

        // "== fresh via ...": first load through the odd spelling, then the
        // canonical spelling, the base name and the odd spelling all find it,
        // and the module's path is the canonical one.
        const struct { const char* odd; const char* canon; const char* base; } fresh[] = {
            {"C:\\program\\.\\fresh_dot.dll", "C:\\program\\fresh_dot.dll", "fresh_dot.dll"},
            {"C:\\program\\nonexistent_dir\\..\\fresh_missing.dll", "C:\\program\\fresh_missing.dll",
             "fresh_missing.dll"},
            {"C:\\program\\\\fresh_dsep.dll", "C:\\program\\fresh_dsep.dll", "fresh_dsep.dll"},
            {"C:\\program/fresh_slash.dll", "C:\\program\\fresh_slash.dll", "fresh_slash.dll"},
            {"C:\\program\\fresh_tdot.dll.", "C:\\program\\fresh_tdot.dll", "fresh_tdot.dll"},
        };
        for (const auto& f : fresh) {
            SCOPED_TRACE(f.odd);
            EXPECT_EQ(modules.get_module_handle(f.base), 0u);
            const GuestAddr h = modules.load_library(f.odd);
            ASSERT_NE(h, 0u);
            const std::string* path = modules.module_path(h);
            ASSERT_NE(path, nullptr);
            EXPECT_EQ(*path, f.canon);
            EXPECT_EQ(modules.get_module_handle(f.odd), h);
            EXPECT_EQ(modules.get_module_handle(f.canon), h);
            EXPECT_EQ(modules.get_module_handle(f.base), h);
            EXPECT_EQ(modules.load_library(f.canon), h);
        }
        EXPECT_EQ(modules.module_count(), 6u);
        std::filesystem::remove_all(root_path);
    }
}

// Learn "Dynamic-link library search order", last step: the directories in
// PATH. A DLL reachable only through PATH loads by bare name
// (tools/oracle_probe/dll_search_path_probe.expected) -- unless
// SetDefaultDllDirectories has replaced the standard order.
TEST(ModuleManagerSearchOrderTest, BareNameLoadSearchesPathDirectories) {
    char root_template[] = "/tmp/vitreo_dll_path_XXXXXX";
    const char* root = ::mkdtemp(root_template);
    ASSERT_NE(root, nullptr);
    const std::filesystem::path root_path(root);
    std::filesystem::create_directories(root_path / "plugins");
    const auto dll_bytes = build_notification_test_dll(/*is_64bit=*/true);
    {
        std::ofstream output(root_path / "plugins" / "onpath.dll", std::ios::binary);
        output.write(reinterpret_cast<const char*>(dll_bytes.data()),
                     static_cast<std::streamsize>(dll_bytes.size()));
    }

    GuestMemory memory;
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
    ThunkRegistry registry(0x00007FFB00000000ull);
    ModuleManager modules(registry);
    modules.attach_dynamic_loader(memory, vfs, /*is_64bit_process=*/true);
    modules.set_main_module(0x0000000140000000ull, "C:\test.exe");

    std::string path_variable = "C:\\nowhere";
    modules.set_path_variable_provider([&] { return path_variable; });
    EXPECT_EQ(modules.load_library("onpath.dll"), 0u);

    // Restricted order: PATH is not in any LOAD_LIBRARY_SEARCH_* set.
    path_variable = "C:\\nowhere;C:\\plugins";
    modules.set_default_dll_directories(0x00001000);  // SEARCH_DEFAULT_DIRS
    EXPECT_EQ(modules.load_library("onpath.dll"), 0u);

    modules.set_default_dll_directories(0);
    EXPECT_NE(modules.load_library("onpath.dll"), 0u);

    std::filesystem::remove_all(root_path);
}

TEST(ModuleManagerBitnessTest, LoadLibraryRejectsWrongMachineType) {
    // Learn process interoperability / LoadLibrary: a 64-bit process cannot
    // load a 32-bit DLL. The file is present; GetLastError is
    // ERROR_BAD_EXE_FORMAT (193), not ERROR_MOD_NOT_FOUND.
    char root_template[] = "/tmp/vitreo_dll_machine_XXXXXX";
    const char* root = ::mkdtemp(root_template);
    ASSERT_NE(root, nullptr);
    const std::filesystem::path root_path(root);
    const auto dll_bytes = build_notification_test_dll(/*is_64bit=*/false);
    {
        std::ofstream output(root_path / "wrong.dll", std::ios::binary);
        output.write(reinterpret_cast<const char*>(dll_bytes.data()),
                     static_cast<std::streamsize>(dll_bytes.size()));
    }

    GuestMemory memory;
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
    ThunkRegistry registry(0x00007FFB00000000ull);
    ModuleManager modules(registry);
    modules.attach_dynamic_loader(memory, vfs, /*is_64bit_process=*/true);

    const GuestAddr base =
        modules.load_library("C:\\wrong.dll");
    EXPECT_EQ(base, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_BAD_EXE_FORMAT);
}

// Learn LoadLibraryW: loading an executable module (.exe, no IMAGE_FILE_DLL)
// is equivalent to LoadLibraryEx(..., DONT_RESOLVE_DLL_REFERENCES) — map the
// image, leave the IAT alone, and do not call the entry point. Measured by
// tools/oracle_probe/loader_contract_probe.expected "== exe as library".
TEST(ModuleManagerBitnessTest, LoadLibraryOfExeSkipsImportsAndEntryPoint) {
    char root_template[] = "/tmp/vitreo_exe_as_lib_XXXXXX";
    const char* root = ::mkdtemp(root_template);
    ASSERT_NE(root, nullptr);
    const std::filesystem::path root_path(root);

    auto exe_bytes = build_notification_test_dll(/*is_64bit=*/true);
    {
        constexpr u32 pe_offset = 0x80;
        auto* coff = reinterpret_cast<pe::CoffHeader*>(
            exe_bytes.data() + pe_offset + sizeof(u32));
        coff->Characteristics = pe::CoffCharacteristics::EXECUTABLE_IMAGE;
        auto* optional = reinterpret_cast<pe::OptionalHeader64*>(
            exe_bytes.data() + pe_offset + sizeof(u32) + sizeof(pe::CoffHeader));
        optional->AddressOfEntryPoint = 0x1000;
    }
    {
        std::ofstream output(root_path / "probe.exe", std::ios::binary);
        output.write(reinterpret_cast<const char*>(exe_bytes.data()),
                     static_cast<std::streamsize>(exe_bytes.size()));
    }

    GuestMemory memory;
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
    ThunkRegistry registry(0x00007FFB00000000ull);
    ModuleManager modules(registry);
    modules.attach_dynamic_loader(memory, vfs, /*is_64bit_process=*/true);

    u32 entry_calls = 0;
    modules.set_guest_call(
        [&entry_calls](GuestAddr, const u64*, usize, bool) -> u64 {
            ++entry_calls;
            return 1;
        });

    set_last_error(0xDEAD);
    const GuestAddr base = modules.load_library("C:\\probe.exe");
    ASSERT_NE(base, 0u);
    EXPECT_EQ(entry_calls, 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_TRUE(modules.free_library(base));

    // DONT_RESOLVE_DLL_REFERENCES on a DLL also skips DllMain (Learn).
    auto dll_bytes = build_notification_test_dll(/*is_64bit=*/true);
    {
        constexpr u32 pe_offset = 0x80;
        auto* optional = reinterpret_cast<pe::OptionalHeader64*>(
            dll_bytes.data() + pe_offset + sizeof(u32) + sizeof(pe::CoffHeader));
        optional->AddressOfEntryPoint = 0x1000;
        std::ofstream output(root_path / "probe.dll", std::ios::binary);
        output.write(reinterpret_cast<const char*>(dll_bytes.data()),
                     static_cast<std::streamsize>(dll_bytes.size()));
    }
    entry_calls = 0;
    const GuestAddr dll = modules.load_library("C:\\probe.dll", 0x1u);
    ASSERT_NE(dll, 0u);
    EXPECT_EQ(entry_calls, 0u);
    EXPECT_TRUE(modules.free_library(dll));

    std::filesystem::remove_all(root_path);
}

TEST_F(ModuleManagerTest,
       SecuritySystemFunctionsApiSetResolvesAdvapi32Export) {
    DllExports advapi_exports;
    advapi_exports.entries.push_back(
        ExportEntry{"SystemFunction036", 1, [](Marshaler&) {}});
    ASSERT_TRUE(
        registry_->register_dll("advapi32.dll", std::move(advapi_exports))
            .is_ok());

    GuestAddr base =
        mm_->load_library("api-ms-win-security-systemfunctions-l1-1-0");
    ASSERT_NE(base, 0u);
    EXPECT_NE(mm_->get_proc_address(base, "SystemFunction036"), 0u);
}

TEST_F(ModuleManagerTest, PowerBaseApiSetResolvesPowrprofExport) {
    DllExports powrprof_exports;
    powrprof_exports.entries.push_back(
        ExportEntry{"CallNtPowerInformation", 0, [](Marshaler&) {}, 20});
    ASSERT_TRUE(
        registry_->register_dll("powrprof.dll", std::move(powrprof_exports))
            .is_ok());

    GuestAddr base =
        mm_->load_library("api-ms-win-power-base-l1-1-0.dll");
    ASSERT_NE(base, 0u);
    EXPECT_NE(mm_->get_proc_address(base, "CallNtPowerInformation"), 0u);
}
