/// @file test_module.cpp
/// @brief Tests for ModuleManager (WIN32-033): module handle lookup, identity, file names, registration

#include "module_test_fixture.hpp"

// =============================================================================
// GetModuleHandle Tests
// =============================================================================

// The module tables are the loader's LdrpModuleDatatableLock domain: a
// LoadLibrary on one thread inserts (and rehashes modules_by_name_) while
// every other thread's GetModuleHandle / GetProcAddress / RtlPcToFileHeader
// walks it. Chromium does exactly this on every DLL it delay-loads. 24k
// registrations across 8 threads force dozens of rehashes under concurrent
// lookups; with no table lock this is a torn bucket walk (host SIGSEGV, the
// FileMappingManager shape from 2026-09-30). free_library never erases a
// thunk-backed module, so this covers growth, the shape that crashes.
TEST_F(ModuleManagerTest, TablesSurviveConcurrentRegisterAndLookup) {
    constexpr int kThreads = 8;
    constexpr int kRounds = 3000;
    constexpr GuestAddr kRaceBase = 0x1'0000'0000ULL;
    constexpr GuestAddr kSlot = 0x10000;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            std::vector<std::string> mine;
            mine.reserve(kRounds);
            for (int i = 0; i < kRounds; ++i) {
                const std::string name = "race_" + std::to_string(t) + "_" +
                                         std::to_string(i) + ".dll";
                const GuestAddr base =
                    kRaceBase + static_cast<GuestAddr>(t * kRounds + i) * kSlot;
                mm_->register_module(name, base, "C:\\Windows\\System32\\" + name);
                mine.push_back(name);
                // Lookups against a module another thread is registering
                // right now, against our own, and against the fixed one.
                if (mm_->get_module_handle(name) != base) ++failures;
                if (mm_->get_module_handle("kernel32.dll") !=
                    ModuleManager::KERNEL32_BASE) ++failures;
                const std::string& other = mine[static_cast<usize>(i) / 2];
                if (mm_->get_module_handle(other) == 0) ++failures;
                if (mm_->get_proc_address(ModuleManager::KERNEL32_BASE,
                                          "TestFunc") == 0) ++failures;
                if (mm_->get_module_info_by_handle(base) == nullptr) ++failures;
                if (mm_->module_for_address(base + 0x10).info == nullptr) ++failures;
                char path[260];
                if (mm_->get_module_filename(base, path, sizeof(path)) == 0) ++failures;
                (void)mm_->module_count();
                if (!mm_->acquire_module_reference(base, false)) ++failures;
            }
        });
    }
    for (auto& w : workers) w.join();
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(mm_->module_count(), 1u + kThreads * kRounds);
}

TEST_F(ModuleManagerTest, GetModuleHandleNullReturnsMainExe) {
    EXPECT_EQ(mm_->get_module_handle(""), MAIN_EXE_BASE);
}

TEST_F(ModuleManagerTest, GetModuleHandleEmptyStringReturnsMainExe) {
    EXPECT_EQ(mm_->get_module_handle(std::string_view{}), MAIN_EXE_BASE);
}

TEST_F(ModuleManagerTest, GetModuleHandleForRegisteredDll) {
    EXPECT_EQ(mm_->get_module_handle("kernel32.dll"), ModuleManager::KERNEL32_BASE);
}

TEST_F(ModuleManagerTest, GetModuleHandleWithoutDllSuffix) {
    EXPECT_EQ(mm_->get_module_handle("kernel32"), ModuleManager::KERNEL32_BASE);
}

TEST_F(ModuleManagerTest, GetModuleHandleCaseInsensitive) {
    EXPECT_EQ(mm_->get_module_handle("Kernel32.dll"), ModuleManager::KERNEL32_BASE);
    EXPECT_EQ(mm_->get_module_handle("KERNEL32.DLL"), ModuleManager::KERNEL32_BASE);
    EXPECT_EQ(mm_->get_module_handle("KERNEL32"), ModuleManager::KERNEL32_BASE);
}

TEST_F(ModuleManagerTest, GetModuleHandleUnknownDllReturnsZero) {
    EXPECT_EQ(mm_->get_module_handle("nonexistent.dll"), 0u);
}

// A host-implemented DLL bound by static import is a loaded module:
// s4_static_import_module_probe on Windows 11 (x86/x64) finds a statically
// imported setupapi.dll by GetModuleHandleA under any case, with or without
// ".dll", and LoadLibraryA returns the same handle. Vitreo bound the import
// to thunks without registering a module, so GetModuleHandle returned NULL.
TEST_F(ModuleManagerTest, StaticImportOfHostDllIsFoundByGetModuleHandle) {
    DllExports setupapi;
    setupapi.entries.push_back(
        ExportEntry{"SetupDiDestroyDeviceInfoList", 0, [](Marshaler&) {}, 4});
    ASSERT_TRUE(registry_->register_dll("setupapi.dll", std::move(setupapi)).is_ok());

    EXPECT_EQ(mm_->get_module_handle("setupapi.dll"), 0u);  // not imported yet
    const GuestAddr base = mm_->ensure_static_import_module("SETUPAPI.dll");
    ASSERT_NE(base, 0u);
    for (const char* name : {"setupapi.dll", "SETUPAPI.DLL", "SetupAPI.dll",
                             "setupapi", "SETUPAPI"}) {
        EXPECT_EQ(mm_->get_module_handle(name), base) << name;
    }
    EXPECT_EQ(mm_->ensure_static_import_module("setupapi.dll"), base);
    EXPECT_EQ(mm_->load_library("setupapi.dll"), base);
    // Already-registered DLLs keep their identity; unknown DLLs stay absent.
    EXPECT_EQ(mm_->ensure_static_import_module("KERNEL32.dll"),
              ModuleManager::KERNEL32_BASE);
    EXPECT_EQ(mm_->ensure_static_import_module("nosuchhostdll.dll"), 0u);
    EXPECT_EQ(mm_->get_module_handle("nosuchhostdll.dll"), 0u);
}

TEST(ModuleManagerKernelBaseTest, LoadedModuleResolvesItsOwnExport) {
    ThunkRegistry registry(0x70000000);
    DllExports exports;
    exports.entries.push_back(
        ExportEntry{"VirtualAlloc2", 1, [](Marshaler&) {}, 28});
    ASSERT_TRUE(registry.register_dll(
        "kernelbase.dll", std::move(exports)).is_ok());

    ModuleManager modules(registry);
    modules.register_module(
        "kernelbase.dll", KERNELBASE_BASE,
        "C:\\Windows\\System32\\kernelbase.dll");
    EXPECT_EQ(modules.get_module_handle("KernelBase.dll"), KERNELBASE_BASE);
    EXPECT_EQ(
        modules.get_proc_address(KERNELBASE_BASE, "VirtualAlloc2"),
        registry.get_thunk_address(
            "kernelbase.dll", "VirtualAlloc2").value());
}

TEST_F(ModuleManagerTest, DllWithSameStemAsMainExeHasDistinctIdentity) {
    constexpr GuestAddr DLL_BASE = 0x18000000;

    DllExports dll_exports;
    dll_exports.entries.push_back(ExportEntry{"GetLibraryVersion", 1, [](Marshaler&) {}});
    ASSERT_TRUE(registry_->register_dll("test.dll", std::move(dll_exports)).is_ok());

    mm_->register_main_module_exports(
        MAIN_EXE_BASE, "C:\\Program Files\\test.exe",
        MAIN_EXE_BASE + 0x10000, 32, {});
    mm_->register_module("test.dll", DLL_BASE, "C:\\Program Files\\test.dll");

    EXPECT_EQ(mm_->get_module_handle("test.exe"), MAIN_EXE_BASE);
    EXPECT_EQ(mm_->get_module_handle("test.dll"), DLL_BASE);
    EXPECT_EQ(mm_->load_library("test.dll"), DLL_BASE);
}

// =============================================================================
// GetModuleFileName Tests
// =============================================================================

TEST_F(ModuleManagerTest, GetModuleFileNameMainExe) {
    char buf[512];
    u32 len = mm_->get_module_filename(0, buf, sizeof(buf));
    EXPECT_GT(len, 0u);
    EXPECT_STREQ(buf, "C:\\Program Files\\test.exe");
}

TEST_F(ModuleManagerTest, GetModuleFileNameMainExeByBase) {
    char buf[512];
    u32 len = mm_->get_module_filename(MAIN_EXE_BASE, buf, sizeof(buf));
    EXPECT_GT(len, 0u);
    EXPECT_STREQ(buf, "C:\\Program Files\\test.exe");
}

TEST_F(ModuleManagerTest, GetModuleFileNameRegisteredDll) {
    char buf[512];
    u32 len = mm_->get_module_filename(ModuleManager::KERNEL32_BASE, buf, sizeof(buf));
    EXPECT_GT(len, 0u);
    EXPECT_STREQ(buf, "C:\\Windows\\System32\\kernel32.dll");
}

TEST_F(ModuleManagerTest, GetModuleFileNameInsufficientBuffer) {
    char buf[8];
    u32 len = mm_->get_module_filename(MAIN_EXE_BASE, buf, sizeof(buf));
    // Path is longer than 8 chars — should truncate and return size
    EXPECT_EQ(len, sizeof(buf));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
}

TEST_F(ModuleManagerTest, GetModuleFileNameUnknownModuleReturnsZero) {
    char buf[512];
    u32 len = mm_->get_module_filename(0xDEADBEEF, buf, sizeof(buf));
    EXPECT_EQ(len, 0u);
}

TEST_F(ModuleManagerTest, GetModuleFileNameNullBufferReturnsZero) {
    u32 len = mm_->get_module_filename(MAIN_EXE_BASE, nullptr, 256);
    EXPECT_EQ(len, 0u);
}

TEST_F(ModuleManagerTest, GetModuleFileNameZeroSizeReturnsZero) {
    char buf[1];
    u32 len = mm_->get_module_filename(MAIN_EXE_BASE, buf, 0);
    EXPECT_EQ(len, 0u);
}

// =============================================================================
// Registration / Module Count
// =============================================================================

TEST_F(ModuleManagerTest, RegisterModuleAndLookup) {
    mm_->register_module("user32.dll", ModuleManager::USER32_BASE,
                         "C:\\Windows\\System32\\user32.dll");
    EXPECT_EQ(mm_->get_module_handle("user32.dll"), ModuleManager::USER32_BASE);
}

TEST_F(ModuleManagerTest, ModuleCountReflectsRegistrations) {
    usize before = mm_->module_count();
    mm_->register_module("user32.dll", ModuleManager::USER32_BASE,
                         "C:\\Windows\\System32\\user32.dll");
    EXPECT_EQ(mm_->module_count(), before + 1);
}

// The loaded-module scans compare each module's name against a normalized key
// without building the normalized string. It must agree with normalize_name
// exactly: basename, lowercase, and one trailing ".dll" stripped only from a
// name longer than the suffix (".exe" and ".drv" stay part of the key).
TEST(ModuleNameKeyTest, AllocationFreeMatchAgreesWithNormalization) {
    using vitreo::winapi::ModuleManager;
    struct Case { const char* name; const char* key; bool match; };
    const Case cases[] = {
        {"kernel32.dll", "kernel32", true},
        {"C:\\Windows\\System32\\KERNEL32.DLL", "kernel32", true},
        {"C:/Games/FactoryGameSteam-Core-Win64-Shipping.dll",
         "factorygamesteam-core-win64-shipping", true},
        {"C:\\a/b\\Mixed.Dll", "mixed", true},
        {"kernel32", "kernel32", true},
        {"kernel32.dll", "kernel32.dll", false},
        {"foo.exe", "foo.exe", true},
        {"foo.exe", "foo", false},
        {"winspool.drv", "winspool.drv", true},
        {"winspool.drv", "winspool", false},
        {"a.dll", "a", true},
        {".dll", ".dll", true},   // not longer than the suffix: kept
        {".dll", "", false},
        {"x.dll.dll", "x.dll", true},  // one suffix only
        {"dir\\sub\\", "", true},
        {"kernel32.dll", "kernel3", false},
        {"kernel32.dll", "kernel32x", false},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(ModuleManager::normalized_name_equals(c.name, c.key), c.match)
            << c.name << " vs " << c.key;
    }
}
