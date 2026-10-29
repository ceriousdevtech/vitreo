/// @file test_module_import_cycles.cpp
/// @brief Tests for ModuleManager import cycles and address lookup during DLL initialization

#include "module_test_fixture.hpp"

#include <optional>

// =============================================================================
// Import cycles (PE/COFF ".idata"; Learn "Dynamic-Link Library Initialization")
// =============================================================================
//
// Nothing in the PE format forbids two DLLs from importing each other, and
// modular Unreal builds do exactly that (Engine <-> Renderer <-> UMG ...). The
// loader maps a module and makes it visible before snapping its imports, so the
// back-reference binds to the already-mapped image; it snaps every import of
// the load before running any initializer, then initializes dependencies
// first. Vitreo used to register a module only after its own imports were
// bound, so the back-reference tried to load the module a second time, failed,
// and Satisfactory's shipping exe could not start.

namespace {

/// A DLL at @p image_base that exports @p export_name (code at RVA 0x1010) and
/// imports @p import_name from @p import_dll through one IAT slot at RVA
/// 0x1260. Its entry point (DllMain) is RVA 0x1000. Two of these naming each
/// other form the smallest import cycle: a.dll -> b.dll -> a.dll.
std::vector<u8> build_cycle_test_dll(bool is_64bit, u64 image_base,
                                     const char* self_name,
                                     const char* export_name,
                                     const char* import_dll,
                                     const char* import_name) {
    auto data = build_notification_test_dll(is_64bit);
    data.resize(0x600, 0);
    constexpr u32 pe_offset = 0x80;
    constexpr u32 section_offset = 0x200;
    constexpr u32 export_rva = 0x1100;
    constexpr u32 import_rva = 0x1200;
    constexpr u32 ilt_rva = 0x1240;
    constexpr u32 iat_rva = 0x1260;
    constexpr u32 hint_name_rva = 0x1280;
    constexpr u32 import_dll_name_rva = 0x12A0;
    const u32 thunk_size = is_64bit ? 8u : 4u;

    const usize optional_offset =
        pe_offset + sizeof(u32) + sizeof(pe::CoffHeader);
    if (is_64bit) {
        auto* optional = reinterpret_cast<pe::OptionalHeader64*>(
            data.data() + optional_offset);
        optional->ImageBase = image_base;
        optional->AddressOfEntryPoint = 0x1000;
        optional->DataDirectory[pe::DIR_EXPORT] = {export_rva, 0x100};
        optional->DataDirectory[pe::DIR_IMPORT] = {import_rva, 40};
        optional->DataDirectory[pe::DIR_IAT] = {iat_rva, 2 * thunk_size};
    } else {
        auto* optional = reinterpret_cast<pe::OptionalHeader32*>(
            data.data() + optional_offset);
        optional->ImageBase = static_cast<u32>(image_base);
        optional->AddressOfEntryPoint = 0x1000;
        optional->DataDirectory[pe::DIR_EXPORT] = {export_rva, 0x100};
        optional->DataDirectory[pe::DIR_IMPORT] = {import_rva, 40};
        optional->DataDirectory[pe::DIR_IAT] = {iat_rva, 2 * thunk_size};
    }
    auto* coff = reinterpret_cast<pe::CoffHeader*>(
        data.data() + pe_offset + sizeof(u32));
    auto* section = reinterpret_cast<pe::SectionHeader*>(
        data.data() + optional_offset + coff->SizeOfOptionalHeader);
    section->VirtualSize = 0x400;
    section->SizeOfRawData = 0x400;
    section->Characteristics |= pe::SCN_MEM_WRITE;

    const auto file_of = [&](u32 rva) {
        return static_cast<usize>(section_offset + (rva - 0x1000));
    };
    const auto put32 = [&](u32 rva, u32 value) {
        std::memcpy(data.data() + file_of(rva), &value, sizeof(value));
    };
    const auto put16 = [&](u32 rva, u16 value) {
        std::memcpy(data.data() + file_of(rva), &value, sizeof(value));
    };
    const auto put_str = [&](u32 rva, const char* text) {
        std::memcpy(data.data() + file_of(rva), text, std::strlen(text) + 1);
    };
    data[file_of(0x1000)] = 0xC3;  // DllMain
    data[file_of(0x1010)] = 0xC3;  // the export

    pe::ExportDirectory dir{};
    dir.Name = 0x1180;
    dir.Base = 1;
    dir.NumberOfFunctions = 1;
    dir.NumberOfNames = 1;
    dir.AddressOfFunctions = 0x1130;
    dir.AddressOfNames = 0x1138;
    dir.AddressOfNameOrdinals = 0x1140;
    std::memcpy(data.data() + file_of(export_rva), &dir, sizeof(dir));
    put32(0x1130, 0x1010);
    put32(0x1138, 0x1150);
    put16(0x1140, 0);
    put_str(0x1150, export_name);
    put_str(0x1180, self_name);

    // IMAGE_IMPORT_DESCRIPTOR { OriginalFirstThunk, TimeDateStamp,
    // ForwarderChain, Name, FirstThunk }, then an all-zero terminator.
    put32(import_rva + 0, ilt_rva);
    put32(import_rva + 12, import_dll_name_rva);
    put32(import_rva + 16, iat_rva);
    put32(ilt_rva, hint_name_rva);   // high bit clear: import by name
    put32(iat_rva, hint_name_rva);
    put16(hint_name_rva, 0);
    put_str(hint_name_rva + 2, import_name);
    put_str(import_dll_name_rva, import_dll);
    return data;
}

struct CycleFixture {
    explicit CycleFixture(bool is_64bit_in) : is_64bit(is_64bit_in) {
        char root_template[] = "/tmp/vitreo_import_cycle_XXXXXX";
        const char* made = ::mkdtemp(root_template);
        EXPECT_NE(made, nullptr);
        root = made;
        a_base = is_64bit ? 0x0000000180000000ull : 0x18000000ull;
        b_base = is_64bit ? 0x0000000190000000ull : 0x19000000ull;
        write_test_file(root / "a.dll",
                        build_cycle_test_dll(is_64bit, a_base, "a.dll",
                                             "FuncA", "b.dll", "FuncB"));
        write_test_file(root / "b.dll",
                        build_cycle_test_dll(is_64bit, b_base, "b.dll",
                                             "FuncB", "a.dll", "FuncA"));
        EXPECT_TRUE(vfs.mount_drive(
            'C', std::make_unique<capsule::HostFileSystem>(root.c_str()))
                        .is_ok());
        modules.attach_dynamic_loader(memory, vfs, is_64bit);
    }
    ~CycleFixture() { std::filesystem::remove_all(root); }

    GuestAddr iat(GuestAddr base) {
        return is_64bit ? memory.read_u64(base + 0x1260).value_or(0)
                        : memory.read_u32(base + 0x1260).value_or(0);
    }

    bool is_64bit;
    std::filesystem::path root;
    GuestAddr a_base{};
    GuestAddr b_base{};
    GuestMemory memory;
    capsule::Vfs vfs;
    ThunkRegistry registry{is_64bit ? 0x00007FFB00000000ull : 0x70000000ull};
    ModuleManager modules{registry};
};

struct EntryCall {
    GuestAddr entry;
    u64 reason;
    GuestAddr a_iat;  // a.dll's IAT slot at the moment of the call
    GuestAddr b_iat;  // b.dll's IAT slot at the moment of the call
};

}  // namespace

TEST_P(DllNotificationDispatchTest,
       AddressLookupIncludesInitializingImagesAndExcludesFailedLoads) {
    // RtlPcToFileHeader searches all mapped images (Microsoft Learn), so
    // dependency and current-image addresses must resolve during DllMain.
    CycleFixture f(GetParam());
    unsigned attaches = 0;
    f.modules.set_guest_call([&](GuestAddr, const u64* args, usize,
                                 bool) -> u64 {
        if (args[1] != 1) return 1;
        ++attaches;
        for (const GuestAddr base : {f.a_base, f.b_base}) {
            const auto first = f.modules.module_for_address(base);
            EXPECT_NE(first.info, nullptr);
            if (!first.info) continue;
            EXPECT_EQ(first.info->base_address, base);
            EXPECT_EQ(first.rva, 0u);
            const auto end = first.info->image_end;
            EXPECT_NE(f.modules.module_for_address(end - 1).info, nullptr);
            EXPECT_EQ(f.modules.module_for_address(end).info, nullptr);
            EXPECT_EQ(f.modules.module_for_address(base - 1).info, nullptr);
        }
        return 0;  // Failed initialization must remove the mapped images.
    });
    EXPECT_EQ(f.modules.load_library("C:\\a.dll"), 0u);
    EXPECT_EQ(attaches, 1u);
    EXPECT_EQ(f.modules.module_for_address(f.a_base).info, nullptr);
    EXPECT_EQ(f.modules.module_for_address(f.b_base).info, nullptr);
    EXPECT_EQ(f.modules.module_for_address(0).info, nullptr);
}

TEST_P(DllNotificationDispatchTest,
       ImportCycleBindsBothWaysBeforeEitherDllMainRuns) {
    CycleFixture f(GetParam());
    std::vector<EntryCall> calls;
    f.modules.set_guest_call([&](GuestAddr entry, const u64* args, usize,
                                 bool) -> u64 {
        calls.push_back({entry, args[1], f.iat(f.a_base), f.iat(f.b_base)});
        return 1;
    });

    const GuestAddr a = f.modules.load_library("C:\\a.dll");
    ASSERT_EQ(a, f.a_base);
    const GuestAddr b = f.modules.get_module_handle("b.dll");
    ASSERT_EQ(b, f.b_base);

    // Both directions of the cycle are bound to the other image's export.
    EXPECT_EQ(f.iat(a), b + 0x1010);
    EXPECT_EQ(f.iat(b), a + 0x1010);
    EXPECT_EQ(f.modules.get_proc_address(a, "FuncA"), a + 0x1010);
    EXPECT_EQ(f.modules.get_proc_address(b, "FuncB"), b + 0x1010);

    // Each DllMain ran once, the dependency first, and neither ran until the
    // whole cycle was bound: when b.dll attached, a.dll's own import of FuncB
    // had already been snapped.
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].entry, b + 0x1000);
    EXPECT_EQ(calls[0].reason, 1u);
    EXPECT_EQ(calls[0].a_iat, b + 0x1010);
    EXPECT_EQ(calls[0].b_iat, a + 0x1010);
    EXPECT_EQ(calls[1].entry, a + 0x1000);
    EXPECT_EQ(calls[1].reason, 1u);

    // A second load is a reference, not a second mapping or attach.
    EXPECT_EQ(f.modules.load_library("C:\\a.dll"), a);
    EXPECT_EQ(f.modules.load_library("b.dll"), b);
    EXPECT_EQ(calls.size(), 2u);
}

TEST_P(DllNotificationDispatchTest,
       ImportCycleDllMainFalseUnloadsTheCycleAndALaterLoadRetries) {
    CycleFixture f(GetParam());
    std::vector<EntryCall> calls;
    bool b_fails = true;
    f.modules.set_guest_call([&](GuestAddr entry, const u64* args, usize,
                                 bool) -> u64 {
        calls.push_back({entry, args[1], 0, 0});
        return (b_fails && entry == f.b_base + 0x1000 && args[1] == 1) ? 0 : 1;
    });

    set_last_error(0);
    EXPECT_EQ(f.modules.load_library("C:\\a.dll"), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_DLL_INIT_FAILED);
    // b.dll attached, failed and got DLL_PROCESS_DETACH; a.dll, whose
    // initializer never ran, is not detached. Both are gone.
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].entry, f.b_base + 0x1000);
    EXPECT_EQ(calls[0].reason, 1u);
    EXPECT_EQ(calls[1].entry, f.b_base + 0x1000);
    EXPECT_EQ(calls[1].reason, 0u);
    EXPECT_EQ(f.modules.get_module_handle("a.dll"), 0u);
    EXPECT_EQ(f.modules.get_module_handle("b.dll"), 0u);

    // Like a plain DllMain FALSE, the failure is not cached.
    b_fails = false;
    calls.clear();
    const GuestAddr a = f.modules.load_library("C:\\a.dll");
    ASSERT_EQ(a, f.a_base);
    EXPECT_EQ(f.iat(a), f.b_base + 0x1010);
    EXPECT_EQ(f.iat(f.b_base), a + 0x1010);
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].entry, f.b_base + 0x1000);
    EXPECT_EQ(calls[1].entry, a + 0x1000);
}

// Process start-up: the main EXE's static imports are bound before any guest
// code can run, and their DllMains are deferred to call_deferred_dll_mains.
TEST_P(DllNotificationDispatchTest, ImportCycleResolvesDuringStaticImportBinding) {
    CycleFixture f(GetParam());
    const GuestAddr a = f.modules.load_library("C:\\a.dll");
    ASSERT_EQ(a, f.a_base);
    ASSERT_EQ(f.modules.get_module_handle("b.dll"), f.b_base);
    EXPECT_EQ(f.iat(a), f.b_base + 0x1010);
    EXPECT_EQ(f.iat(f.b_base), a + 0x1010);

    std::vector<GuestAddr> attached;
    f.modules.set_guest_call([&](GuestAddr entry, const u64* args, usize,
                                 bool) -> u64 {
        if (args[1] == 1) attached.push_back(entry);
        return 1;
    });
    f.modules.call_deferred_dll_mains();
    ASSERT_EQ(attached.size(), 2u);
    EXPECT_NE(attached[0], attached[1]);
    for (GuestAddr entry : attached) {
        EXPECT_TRUE(entry == a + 0x1000 || entry == f.b_base + 0x1000);
    }
}

// =============================================================================
// Dependency search order (Learn "Dynamic-link library search order")
// =============================================================================
//
// "If a DLL has dependencies, then the system searches for the dependent DLLs
// as if they were loaded by using only their module names. That's true even
// if the first DLL was loaded by specifying a full path." So a dependency
// that is in neither the DLL's folder, the application folder nor the
// Windows folders is found in the current folder, then in each PATH
// directory, and SetDllDirectory("") takes the current folder out.

TEST(ModuleDependencySearchOrderTest, DependencyIsFoundInCurrentFolderThenPath) {
    for (const bool is_64bit : {false, true}) {
        SCOPED_TRACE(is_64bit ? "x64" : "x86");
        char root_template[] = "/tmp/vitreo_dep_search_XXXXXX";
        const char* made = ::mkdtemp(root_template);
        ASSERT_NE(made, nullptr);
        const std::filesystem::path root(made);
        const u64 plugin_base = is_64bit ? 0x0000000180000000ull : 0x18000000ull;
        const u64 dep_base = is_64bit ? 0x0000000190000000ull : 0x19000000ull;
        std::filesystem::create_directories(root / "game");
        std::filesystem::create_directories(root / "launcher");
        std::filesystem::create_directories(root / "work");
        const auto plugin = build_cycle_test_dll(is_64bit, plugin_base, "plugin.dll",
                                                 "FuncA", "dep.dll", "FuncB");
        const auto dep = build_cycle_test_dll(is_64bit, dep_base, "dep.dll", "FuncB",
                                              "plugin.dll", "FuncA");
        write_test_file(root / "game" / "plugin.dll", plugin);
        write_test_file(root / "launcher" / "dep.dll", dep);

        auto load_plugin = [&](const char* path_variable, const char* current_directory,
                               std::optional<std::string> dll_directory) {
            GuestMemory memory;
            capsule::Vfs vfs;
            EXPECT_TRUE(vfs.mount_drive(
                'C', std::make_unique<capsule::HostFileSystem>(root.c_str())).is_ok());
            ThunkRegistry registry(is_64bit ? 0x00007FFB00000000ull : 0x70000000ull);
            ModuleManager modules(registry);
            modules.attach_dynamic_loader(memory, vfs, is_64bit);
            modules.set_main_module(is_64bit ? 0x0000000140000000ull : 0x00400000ull,
                                    "C:\\app\\app.exe");
            const std::string path = path_variable;
            modules.set_path_variable_provider([path] { return path; });
            modules.set_current_directory(current_directory);
            if (dll_directory) modules.set_dll_directory(*dll_directory);
            const GuestAddr base = modules.load_library("C:\\game\\plugin.dll");
            if (base == 0) return u64{0};
            return is_64bit ? memory.read_u64(base + 0x1260).value_or(0)
                            : u64{memory.read_u32(base + 0x1260).value_or(0)};
        };

        const u64 func_b = dep_base + 0x1010;
        // Nowhere on the search path: the import cannot bind to dep.dll.
        EXPECT_NE(load_plugin("C:\\nowhere", "C:\\work", std::nullopt), func_b);
        // PATH directory (step 12), with and without a trailing separator.
        EXPECT_EQ(load_plugin("C:\\nowhere;C:\\launcher", "C:\\work", std::nullopt), func_b);
        EXPECT_EQ(load_plugin("\"C:\\launcher\\\"", "C:\\work", std::nullopt), func_b);
        // Current folder (step 11), and SetDllDirectory("") removes it.
        EXPECT_EQ(load_plugin("", "C:\\launcher", std::nullopt), func_b);
        EXPECT_NE(load_plugin("", "C:\\launcher", std::string{}), func_b);
        // The SetDllDirectory directory itself is searched.
        EXPECT_EQ(load_plugin("", "C:\\work", std::string("C:\\launcher")), func_b);
        std::filesystem::remove_all(root);
    }
}
