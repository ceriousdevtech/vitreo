/// @file test_module_synthetic_image.cpp
/// @brief Tests for ModuleManager synthetic module images (PE headers, ntdll service veneers, linear-reservation cap)

#include "module_test_fixture.hpp"
#include "winapi/common/synthetic_version_resource.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/system/ntdll/ntdll_system_services.hpp"

#include <optional>

TEST(ModuleManagerSyntheticImageTest, X86HandleBacksValidPeHeaderOutsideStack) {
    GuestMemory memory;
    capsule::Vfs vfs;
    ThunkRegistry registry(0x70000000);
    ModuleManager modules(registry);
    modules.attach_dynamic_loader(memory, vfs, false);

    modules.register_module(
        "ntdll.dll", NTDLL_BASE, "C:\\Windows\\System32\\ntdll.dll");

    EXPECT_LT(NTDLL_BASE + SYNTHETIC_MODULE_SLOT_SIZE, 0x7F000000u);
    ASSERT_EQ(memory.read_u16(NTDLL_BASE).value(), 0x5A4Du);
    const u32 nt_offset = memory.read_u32(NTDLL_BASE + 0x3C).value();
    ASSERT_EQ(memory.read_u32(NTDLL_BASE + nt_offset).value(), 0x00004550u);
    EXPECT_EQ(memory.read_u16(NTDLL_BASE + nt_offset + 4).value(), 0x014Cu);
    EXPECT_EQ(memory.read_u16(NTDLL_BASE + nt_offset + 20).value(), 0x00E0u);
    EXPECT_EQ(
        memory.read_u32(NTDLL_BASE + nt_offset + 24 + 0x5C).value(), 16u);
}

TEST(ModuleManagerSyntheticImageTest, X86NtdllNtExportUsesSysenterVeneer) {
    GuestMemory memory;
    capsule::Vfs vfs;
    ThunkRegistry registry(0x70000000);
    ModuleManager modules(registry);
    modules.attach_dynamic_loader(memory, vfs, false);
    modules.register_module(
        "ntdll.dll", NTDLL_BASE, "C:\\Windows\\System32\\ntdll.dll");

    DllExports ntdll_exports;
    ntdll_exports.entries.push_back(
        ExportEntry{"NtQueryInformationProcess", 2, [](Marshaler&) {}, 20});
    ASSERT_TRUE(
        registry.register_dll("ntdll.dll", std::move(ntdll_exports)).is_ok());
    modules.materialize_synthetic_modules();

    ASSERT_EQ(modules.get_module_handle("ntdll.dll"), NTDLL_BASE);
    ASSERT_EQ(memory.read_u16(NTDLL_BASE).value(), 0x5A4Du);
    const u32 nt_offset = memory.read_u32(NTDLL_BASE + 0x3C).value();
    ASSERT_EQ(memory.read_u32(NTDLL_BASE + nt_offset).value(), 0x00004550u);

    const GuestAddr optional = NTDLL_BASE + nt_offset + 24;
    const u32 export_rva = memory.read_u32(optional + 0x60).value();
    const u32 export_size = memory.read_u32(optional + 0x64).value();
    ASSERT_NE(export_rva, 0u);
    ASSERT_NE(export_size, 0u);

    const GuestAddr directory = NTDLL_BASE + export_rva;
    const u32 ordinal_base = memory.read_u32(directory + 16).value();
    const u32 function_count = memory.read_u32(directory + 20).value();
    const u32 name_count = memory.read_u32(directory + 24).value();
    const u32 functions_rva = memory.read_u32(directory + 28).value();
    const u32 names_rva = memory.read_u32(directory + 32).value();
    const u32 ordinals_rva = memory.read_u32(directory + 36).value();
    ASSERT_EQ(name_count, 1u);

    GuestAddr resolved_stub = 0;
    for (u32 i = 0; i < name_count; ++i) {
        const u32 name_rva =
            memory.read_u32(NTDLL_BASE + names_rva + i * 4).value();
        std::string name;
        for (u32 j = 0; j < 128; ++j) {
            const u8 byte =
                memory.read_u8(NTDLL_BASE + name_rva + j).value();
            if (byte == 0) break;
            name.push_back(static_cast<char>(byte));
        }
        if (name != "NtQueryInformationProcess") continue;

        const u16 ordinal_index =
            memory.read_u16(NTDLL_BASE + ordinals_rva + i * 2).value();
        ASSERT_LT(ordinal_index, function_count);
        const u32 stub_rva = memory.read_u32(
            NTDLL_BASE + functions_rva + ordinal_index * 4).value();
        EXPECT_EQ(ordinal_base + ordinal_index, 2u);
        EXPECT_GE(stub_rva, export_rva + export_size);
        resolved_stub = NTDLL_BASE + stub_rva;
        break;
    }

    ASSERT_NE(resolved_stub, 0u);
    // Direct PE parsers and GetProcAddress observe the same exported address.
    // The number is the service's rank (its only service: 0), not its
    // ordinal; the stub is padded to WoW64's 16-byte stride with NOPs.
    EXPECT_EQ(memory.read_u8(resolved_stub + 0).value(), 0xB8u);
    EXPECT_EQ(memory.read_u32(resolved_stub + 1).value(), 0u);
    EXPECT_EQ(memory.read_u8(resolved_stub + 5).value(), 0x89u);
    EXPECT_EQ(memory.read_u8(resolved_stub + 6).value(), 0xE2u);
    EXPECT_EQ(memory.read_u16(resolved_stub + 7).value(), 0x340Fu);
    EXPECT_EQ(memory.read_u8(resolved_stub + 9).value(), 0xC2u);
    EXPECT_EQ(memory.read_u16(resolved_stub + 10).value(), 20u);
    for (u32 pad = 12; pad < 16; ++pad) {
        EXPECT_EQ(memory.read_u8(resolved_stub + pad).value(), 0x90u) << pad;
    }
    // SYSENTER enters the service EAX names, so it is not an alias of this
    // stub's own service.
    EXPECT_FALSE(registry.is_dispatch_address(resolved_stub + 7));
    EXPECT_EQ(registry.native_service_thunk(0),
              registry.get_thunk_address("ntdll.dll",
                                         "NtQueryInformationProcess").value());
    EXPECT_EQ(registry.native_service_thunk(1), 0u);
    EXPECT_EQ(modules.get_proc_address(
                  NTDLL_BASE, "NtQueryInformationProcess"),
              resolved_stub);
}

TEST(ModuleManagerSyntheticImageTest, X64HandleBacksValidPeHeaderInHighArena) {
    GuestMemory memory;
    capsule::Vfs vfs;
    ThunkRegistry registry(0x00007FFB'00000000ULL);
    ModuleManager modules(registry);
    modules.attach_dynamic_loader(memory, vfs, true);
    const GuestAddr NTDLL_BASE_X64 =
        modules.synthetic_module_arena() + 0x10000;
    modules.register_module(
        "ntdll.dll", NTDLL_BASE_X64, "C:\\Windows\\System32\\ntdll.dll");

    // RuntimeContext registers well-known module identities before it
    // populates ThunkRegistry. The first image is therefore header-only.
    const u32 initial_nt_offset =
        memory.read_u32(NTDLL_BASE_X64 + 0x3C).value();
    EXPECT_EQ(
        memory.read_u32(
            NTDLL_BASE_X64 + initial_nt_offset + 24 + 0x70).value(),
        0u);

    DllExports ntdll_exports;
    ntdll_exports.entries.push_back(
        ExportEntry{"NtQueryInformationProcess", 2, [](Marshaler&) {}});
    ntdll_exports.entries.push_back(
        ExportEntry{"ZwQueryInformationProcess", 101, [](Marshaler&) {}});
    ntdll_exports.entries.push_back(
        ExportEntry{"NtQuerySection", 100, [](Marshaler&) {}});
    ntdll_exports.entries.push_back(
        ExportEntry{"DbgUserBreakPoint", 102, [](Marshaler&) {}});
    ASSERT_TRUE(
        registry.register_dll("ntdll.dll", std::move(ntdll_exports)).is_ok());
    modules.materialize_synthetic_modules();

    ASSERT_EQ(modules.get_module_handle("ntdll.dll"), NTDLL_BASE_X64);
    ASSERT_EQ(memory.read_u16(NTDLL_BASE_X64).value(), 0x5A4Du);
    const u32 nt_offset = memory.read_u32(NTDLL_BASE_X64 + 0x3C).value();
    ASSERT_EQ(
        memory.read_u32(NTDLL_BASE_X64 + nt_offset).value(), 0x00004550u);
    EXPECT_EQ(
        memory.read_u16(NTDLL_BASE_X64 + nt_offset + 4).value(), 0x8664u);
    // .edata, .text and .rsrc.
    EXPECT_EQ(
        memory.read_u16(NTDLL_BASE_X64 + nt_offset + 6).value(), 3u);
    EXPECT_EQ(
        memory.read_u16(NTDLL_BASE_X64 + nt_offset + 20).value(), 0x00F0u);
    EXPECT_EQ(
        memory.read_u32(NTDLL_BASE_X64 + nt_offset + 24 + 0x6C).value(),
        16u);
    EXPECT_EQ(
        memory.read_u64(NTDLL_BASE_X64 + nt_offset + 24 + 0x18).value(),
        NTDLL_BASE_X64);

    // Guest code is allowed to parse a loaded module's export directory
    // directly instead of calling GetProcAddress. The synthetic image must
    // therefore expose the same named thunk exports as the registry.
    const GuestAddr optional = NTDLL_BASE_X64 + nt_offset + 24;
    const GuestAddr sections = optional + 0xF0;
    const u32 data_raw_size = memory.read_u32(sections + 0x10).value();
    const u32 code_rva = memory.read_u32(sections + 40 + 0x0C).value();
    const u32 code_raw_size =
        memory.read_u32(sections + 40 + 0x10).value();
    const u32 resource_raw_size =
        memory.read_u32(sections + 80 + 0x10).value();
    EXPECT_EQ(memory.read_u32(optional + 0x04).value(), code_raw_size);
    EXPECT_EQ(memory.read_u32(optional + 0x08).value(),
              data_raw_size + resource_raw_size);
    EXPECT_EQ(memory.read_u32(optional + 0x14).value(), code_rva);

    const u32 stored_checksum = memory.read_u32(optional + 0x40).value();
    ASSERT_NE(stored_checksum, 0u);
    u64 computed_checksum = 0;
    for (u32 offset = 0; offset < 0x10000; offset += 2) {
        u16 word = 0;
        if (offset != nt_offset + 24 + 0x40 &&
            offset != nt_offset + 24 + 0x42) {
            word = memory.read_u16(NTDLL_BASE_X64 + offset).value();
        }
        computed_checksum += word;
        computed_checksum =
            (computed_checksum & 0xFFFFu) + (computed_checksum >> 16);
    }
    computed_checksum =
        (computed_checksum & 0xFFFFu) + (computed_checksum >> 16);
    computed_checksum += 0x10000u;
    EXPECT_EQ(stored_checksum, static_cast<u32>(computed_checksum));

    const u32 export_rva = memory.read_u32(optional + 0x70).value();
    const u32 export_size = memory.read_u32(optional + 0x74).value();
    ASSERT_NE(export_rva, 0u);
    ASSERT_NE(export_size, 0u);
    EXPECT_FALSE(memory.has_protection(
        NTDLL_BASE_X64 + export_rva, export_size,
        MemoryProtection::Execute));

    const GuestAddr directory = NTDLL_BASE_X64 + export_rva;
    const u32 ordinal_base = memory.read_u32(directory + 16).value();
    const u32 function_count = memory.read_u32(directory + 20).value();
    const u32 name_count = memory.read_u32(directory + 24).value();
    const u32 functions_rva = memory.read_u32(directory + 28).value();
    const u32 names_rva = memory.read_u32(directory + 32).value();
    const u32 ordinals_rva = memory.read_u32(directory + 36).value();
    ASSERT_EQ(name_count, 4u);

    GuestAddr resolved_stub = 0;
    GuestAddr breakpoint_stub = 0;
    for (u32 i = 0; i < name_count; ++i) {
        const u32 name_rva =
            memory.read_u32(NTDLL_BASE_X64 + names_rva + i * 4).value();
        std::string name;
        for (u32 j = 0; j < 128; ++j) {
            const u8 byte =
                memory.read_u8(NTDLL_BASE_X64 + name_rva + j).value();
            if (byte == 0) break;
            name.push_back(static_cast<char>(byte));
        }
        const u16 ordinal_index =
            memory.read_u16(NTDLL_BASE_X64 + ordinals_rva + i * 2).value();
        ASSERT_LT(ordinal_index, function_count);
        const u32 stub_rva = memory.read_u32(
            NTDLL_BASE_X64 + functions_rva + ordinal_index * 4).value();
        if (name == "DbgUserBreakPoint") {
            breakpoint_stub = NTDLL_BASE_X64 + stub_rva;
            continue;
        }
        if (name != "NtQueryInformationProcess") continue;
        EXPECT_EQ(ordinal_base + ordinal_index, 2u);
        EXPECT_GE(stub_rva, export_rva + export_size);
        resolved_stub = NTDLL_BASE_X64 + stub_rva;
        break;
    }

    ASSERT_NE(resolved_stub, 0u);
    EXPECT_TRUE(memory.has_protection(
        resolved_stub, 1, MemoryProtection::Execute));
    // A direct ntdll export parser observes the canonical x64 native-service
    // prologue used by sandbox interception validators. GetProcAddress must
    // identify this same exported entry point.
    EXPECT_EQ(memory.read_u8(resolved_stub + 0).value(), 0x4Cu);
    EXPECT_EQ(memory.read_u8(resolved_stub + 1).value(), 0x8Bu);
    EXPECT_EQ(memory.read_u8(resolved_stub + 2).value(), 0xD1u);
    EXPECT_EQ(memory.read_u8(resolved_stub + 3).value(), 0xB8u);
    // Its rank among the services (NtQueryInformationProcess, NtQuerySection).
    EXPECT_EQ(memory.read_u32(resolved_stub + 4).value(), 0u);
    EXPECT_EQ(memory.read_u8(resolved_stub + 8).value(), 0xF6u);
    EXPECT_EQ(memory.read_u8(resolved_stub + 9).value(), 0x04u);
    EXPECT_EQ(memory.read_u8(resolved_stub + 10).value(), 0x25u);
    EXPECT_EQ(memory.read_u32(resolved_stub + 11).value(), 0x7FFE0308u);
    EXPECT_EQ(memory.read_u8(resolved_stub + 15).value(), 0x01u);
    EXPECT_EQ(memory.read_u8(resolved_stub + 16).value(), 0x75u);
    EXPECT_EQ(memory.read_u8(resolved_stub + 17).value(), 0x03u);
    EXPECT_EQ(memory.read_u16(resolved_stub + 18).value(), 0x050Fu);
    EXPECT_EQ(memory.read_u8(resolved_stub + 20).value(), 0xC3u);
    EXPECT_EQ(memory.read_u16(resolved_stub + 21).value(), 0x2ECDu);
    EXPECT_EQ(memory.read_u8(resolved_stub + 23).value(), 0xC3u);
    // nop dword ptr [rax+rax+0] to the 32-byte stride.
    EXPECT_EQ(memory.read_u64(resolved_stub + 24).value(),
              0x00000000841F0FULL);
    // The `syscall` enters the service EAX names (an indirect syscall reaches
    // it with another number), so only native64's `int 2Eh` is an alias.
    EXPECT_FALSE(registry.is_dispatch_address(resolved_stub + 18));
    EXPECT_TRUE(registry.is_dispatch_address(resolved_stub + 21));
    EXPECT_EQ(registry.native_service_thunk(0),
              registry.get_thunk_address("ntdll.dll",
                                         "NtQueryInformationProcess").value());
    EXPECT_EQ(modules.get_proc_address(
                  NTDLL_BASE_X64, "NtQueryInformationProcess"),
              resolved_stub);
    EXPECT_EQ(modules.get_proc_address(
                  NTDLL_BASE_X64, "ZwQueryInformationProcess"),
              resolved_stub);

    ASSERT_NE(breakpoint_stub, 0u);
    EXPECT_EQ(memory.read_u8(breakpoint_stub).value(), 0xCCu);
    EXPECT_EQ(memory.read_u8(breakpoint_stub + 1).value(), 0xC3u);
    EXPECT_EQ(modules.get_proc_address(
                  NTDLL_BASE_X64, "DbgUserBreakPoint"),
              breakpoint_stub);
}

TEST(ModuleManagerSyntheticImageTest,
     X64LinearCapKeepsSyntheticAndDynamicModulesInsideReservation) {
    constexpr usize reservation = usize{32} * 1024 * 1024 * 1024;
    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(reservation));
    memory.set_maximum_user_address(reservation - 1);

    char root_template[] = "/tmp/vitreo_dll_linear64_XXXXXX";
    const char* root = ::mkdtemp(root_template);
    ASSERT_NE(root, nullptr);
    const std::filesystem::path root_path(root);
    auto dll_bytes = build_notification_test_dll(/*is_64bit=*/true);
    auto* dos = reinterpret_cast<pe::DosHeader*>(dll_bytes.data());
    auto* optional = reinterpret_cast<pe::OptionalHeader64*>(
        dll_bytes.data() + dos->e_lfanew + sizeof(u32) +
        sizeof(pe::CoffHeader));
    // No preferred base forces ModuleManager's dynamic-placement policy.
    optional->ImageBase = 0;
    {
        std::ofstream output(root_path / "capped.dll", std::ios::binary);
        output.write(reinterpret_cast<const char*>(dll_bytes.data()),
                     static_cast<std::streamsize>(dll_bytes.size()));
    }

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
    ThunkRegistry registry(reservation - 0x00000005'00000000ULL);
    ModuleManager modules(registry);
    modules.attach_dynamic_loader(memory, vfs, true);

    const GuestAddr synthetic = modules.synthetic_module_arena();
    EXPECT_LT(synthetic, reservation);
    modules.register_module(
        "ntdll.dll", synthetic + 0x10000,
        "C:\\Windows\\System32\\ntdll.dll");
    EXPECT_EQ(modules.get_module_handle("ntdll.dll"), synthetic + 0x10000);
    EXPECT_TRUE(memory.read_u16(synthetic + 0x10000).is_ok());

    const GuestAddr dynamic = modules.load_library("C:\\capped.dll");
    ASSERT_NE(dynamic, 0u);
    EXPECT_LT(dynamic, reservation);
    EXPECT_TRUE(memory.read_u16(dynamic).is_ok());
}

namespace {

std::vector<u8> read_vfs_file(capsule::Vfs& vfs, std::string_view path) {
    auto opened = vfs.open(path, capsule::AccessMode::Read,
                           capsule::ShareMode::Read,
                           capsule::CreationDisposition::OpenExisting);
    if (opened.is_err()) return {};
    auto info = vfs.stat_handle(opened.value());
    std::vector<u8> bytes(info.is_ok() ? info.value().size : 0);
    auto got = vfs.read(opened.value(), std::span<u8>(bytes));
    (void)vfs.close(opened.value());
    if (got.is_err()) return {};
    bytes.resize(got.value());
    return bytes;
}

u16 le16(const std::vector<u8>& b, usize at) {
    return static_cast<u16>(b.at(at) | (b.at(at + 1) << 8));
}

u32 le32(const std::vector<u8>& b, usize at) {
    return static_cast<u32>(le16(b, at)) |
           (static_cast<u32>(le16(b, at + 2)) << 16);
}

usize data_directory(const std::vector<u8>& file, u32 index) {
    const u32 nt = le32(file, 0x3C);
    const bool x64 = le16(file, nt + 24) == 0x020B;
    return nt + 24 + (x64 ? 0x70u : 0x60u) + index * 8u;
}

// Every section's raw data is at its RVA and inside the file: the image is
// also a valid file layout, so a parser of the file finds what the loader
// mapped.
bool sections_are_file_backed(const std::vector<u8>& file) {
    const u32 nt = le32(file, 0x3C);
    const u16 count = le16(file, nt + 6);
    const usize table = nt + 24 + le16(file, nt + 20);
    for (u16 i = 0; i < count; ++i) {
        const usize header = table + i * 40u;
        const u32 rva = le32(file, header + 12);
        const u32 raw_size = le32(file, header + 16);
        const u32 raw = le32(file, header + 20);
        if (raw != rva || raw + raw_size > file.size()) return false;
    }
    return count != 0;
}

// The RT_VERSION resource's bytes: type 16, its first name, first language.
std::vector<u8> version_resource_of(const std::vector<u8>& file) {
    const u32 rsrc = le32(file, data_directory(file, 2));
    if (rsrc == 0) return {};
    u32 node = 0;
    for (int level = 0; level < 3; ++level) {
        const usize at = rsrc + node;
        const u32 entries = static_cast<u32>(le16(file, at + 12)) +
                            le16(file, at + 14);
        const u32 named = le16(file, at + 12);
        std::optional<u32> next;
        for (u32 i = 0; i < entries && !next; ++i) {
            const u32 id = le32(file, at + 16 + i * 8);
            if (level == 0 && (i < named || id != 16)) continue;
            next = le32(file, at + 16 + i * 8 + 4);
        }
        if (!next) return {};
        const bool subdirectory = (*next & 0x80000000u) != 0;
        if (subdirectory != (level < 2)) return {};
        node = *next & 0x7FFFFFFFu;
    }
    const u32 data_rva = le32(file, rsrc + node);
    const u32 size = le32(file, rsrc + node + 4);
    if (data_rva + size > file.size()) return {};
    return std::vector<u8>(file.begin() + data_rva,
                           file.begin() + data_rva + size);
}

u32 export_rva(const std::vector<u8>& file, std::string_view wanted) {
    const u32 dir = le32(file, data_directory(file, 0));
    if (dir == 0) return 0;
    const u32 names = le32(file, dir + 24);
    for (u32 i = 0; i < names; ++i) {
        const u32 name_rva = le32(file, le32(file, dir + 32) + i * 4);
        std::string name;
        for (u32 j = name_rva; file.at(j) != 0; ++j) {
            name.push_back(static_cast<char>(file[j]));
        }
        if (name != wanted) continue;
        const u16 index = le16(file, le32(file, dir + 36) + i * 2);
        return le32(file, le32(file, dir + 28) + index * 4u);
    }
    return 0;
}

struct BuiltInDllFiles {
    std::filesystem::path root;
    GuestMemory memory;
    capsule::Vfs vfs;
    ThunkRegistry registry;
    ModuleManager modules;

    explicit BuiltInDllFiles(bool is_64bit)
        : registry(is_64bit ? 0x00007FFB'00000000ULL : 0x70000000),
          modules(registry) {
        char root_template[] = "/tmp/vitreo_builtin_dll_files_XXXXXX";
        root = ::mkdtemp(root_template);
        std::filesystem::create_directories(root / "Windows" / "System32");
        std::filesystem::create_directories(root / "Windows" / "SysWOW64");
        (void)vfs.mount_drive(
            'C', std::make_unique<capsule::HostFileSystem>(root.string()));
        modules.attach_dynamic_loader(memory, vfs, is_64bit);
        // As runtime_context: a 32-bit process sees System32 through WOW64
        // file-system redirection.
        vfs.set_wow64_view(!is_64bit);
        // As RuntimeContext wires it.
        vfs.set_generated_file_source(
            [this](std::string_view path) {
                return modules.builtin_dll_file(path);
            },
            [this](std::string_view directory) {
                return modules.builtin_dll_files_in(directory);
            });
        DllExports dsound;
        dsound.entries.push_back(
            ExportEntry{"DirectSoundCreate", 1, [](Marshaler&) {}, 12});
        (void)registry.register_dll("dsound.dll", std::move(dsound));
        DllExports ntdll;
        ntdll.entries.push_back(ExportEntry{
            "NtQueryInformationProcess", 2, [](Marshaler&) {}, 20});
        (void)registry.register_dll("ntdll.dll", std::move(ntdll));
    }
    ~BuiltInDllFiles() {
        vfs.set_generated_file_source(nullptr, nullptr);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

}  // namespace

// GetModuleFileNameW names C:\Windows\System32 for a built-in DLL, and on
// Windows that path is the module's file: a PE of the loaded machine and
// SizeOfImage, with its version resource (builtin_dll_file_probe). Before
// the module loads, the file already exists; once it loads, the file's
// bytes are the image in memory.
TEST(ModuleManagerSyntheticImageTest,
     BuiltInDllFileIsTheModuleImageWithItsVersionResource) {
    for (const bool is_64bit : {true, false}) {
        SCOPED_TRACE(is_64bit ? "x64" : "x86");
        BuiltInDllFiles env(is_64bit);
        const auto expected_version =
            make_synthetic_system_version_resource("dsound.dll");
        ASSERT_FALSE(expected_version.empty());

        const auto before =
            read_vfs_file(env.vfs, "C:\\Windows\\System32\\dsound.dll");
        ASSERT_EQ(before.size(), SYNTHETIC_MODULE_SLOT_SIZE);
        EXPECT_EQ(le16(before, 0), 0x5A4Du);
        const u32 nt = le32(before, 0x3C);
        EXPECT_EQ(le32(before, nt), 0x00004550u);
        EXPECT_EQ(le16(before, nt + 4), is_64bit ? 0x8664u : 0x014Cu);
        EXPECT_EQ(le32(before, nt + 24 + 0x38), SYNTHETIC_MODULE_SLOT_SIZE);
        EXPECT_TRUE(sections_are_file_backed(before));
        EXPECT_EQ(version_resource_of(before), expected_version);
        EXPECT_NE(export_rva(before, "DirectSoundCreate"), 0u);

        // SysWOW64 holds the x86 file in either process.
        const auto wow =
            read_vfs_file(env.vfs, "c:/windows/SYSWOW64/DSound.dll");
        ASSERT_EQ(wow.size(), SYNTHETIC_MODULE_SLOT_SIZE);
        EXPECT_EQ(le16(wow, le32(wow, 0x3C) + 4), 0x014Cu);
        EXPECT_EQ(version_resource_of(wow), expected_version);

        auto listed = env.vfs.list_dir("C:\\Windows\\System32");
        ASSERT_TRUE(listed.is_ok());
        const auto entry = std::find_if(
            listed.value().begin(), listed.value().end(),
            [](const capsule::DirectoryEntry& e) {
                return e.name == "dsound.dll";
            });
        ASSERT_NE(entry, listed.value().end());
        EXPECT_EQ(entry->info.size, SYNTHETIC_MODULE_SLOT_SIZE);
        EXPECT_EQ(entry->info.attributes, capsule::FileAttributes::Archive);

        EXPECT_EQ(
            read_vfs_file(env.vfs, "C:\\Windows\\System32\\absent.dll").size(),
            0u);
        EXPECT_EQ(read_vfs_file(env.vfs,
                                "C:\\Windows\\System32\\sub\\dsound.dll")
                      .size(),
                  0u);

        // Loading it loads the built-in module, by name or by that path.
        const GuestAddr base = env.modules.load_library("dsound.dll");
        ASSERT_NE(base, 0u);
        EXPECT_EQ(env.modules.load_library("C:\\Windows\\System32\\dsound.dll"),
                  base);
        const auto after =
            read_vfs_file(env.vfs, "C:\\Windows\\System32\\dsound.dll");
        ASSERT_EQ(after.size(), SYNTHETIC_MODULE_SLOT_SIZE);
        std::vector<u8> mapped(SYNTHETIC_MODULE_SLOT_SIZE);
        ASSERT_TRUE(env.memory.read_bytes(base, mapped.data(), mapped.size())
                        .is_ok());
        EXPECT_EQ(after, mapped);
    }
}

// The ntdll service veneers dispatch through aliases registered at the
// image's base. Building a FILE's bytes -- for a module not loaded, or the
// other bitness's -- must not plant those aliases where nothing is mapped.
TEST(ModuleManagerSyntheticImageTest, BuiltInDllFileBuildsRegisterNoDispatchAliases) {
    for (const bool is_64bit : {true, false}) {
        SCOPED_TRACE(is_64bit ? "x64" : "x86");
        BuiltInDllFiles env(is_64bit);
        for (const char* path : {"C:\\Windows\\System32\\ntdll.dll",
                                 "C:\\Windows\\SysWOW64\\ntdll.dll"}) {
            SCOPED_TRACE(path);
            const auto file = read_vfs_file(env.vfs, path);
            ASSERT_EQ(file.size(), SYNTHETIC_MODULE_SLOT_SIZE);
            const u32 nt = le32(file, 0x3C);
            const bool x64 = le16(file, nt + 24) == 0x020B;
            const GuestAddr image_base =
                x64 ? (static_cast<GuestAddr>(le32(file, nt + 24 + 0x1C)) << 32 |
                       le32(file, nt + 24 + 0x18))
                    : le32(file, nt + 24 + 0x1C);
            const u32 stub = export_rva(file, "NtQueryInformationProcess");
            ASSERT_NE(stub, 0u);
            for (const u32 offset : {7u, 18u, 21u}) {
                EXPECT_EQ(env.registry.alias_dispatch_gateway(
                              image_base + stub + offset),
                          0u)
                    << "offset " << offset;
            }
        }
    }
}

TEST_P(DllNotificationDispatchTest,
       NameOnlyNativeServiceIdsResolveToTheirHandlers) {
    const bool is_64bit = GetParam();
    GuestMemory memory;
    capsule::Vfs vfs;
    ThunkRegistry registry(
        is_64bit ? 0x00007FFB00000000ull : 0x70000000ull);
    ModuleManager modules(registry);
    modules.attach_dynamic_loader(memory, vfs, is_64bit);
    DllExports exports;
    exports.entries.push_back(
        ExportEntry{"NtQueryObject", 0, [](Marshaler&) {}, 20});
    exports.entries.push_back(
        ExportEntry{"NtClose", 0, [](Marshaler&) {}, 4});
    exports.entries.push_back(
        ExportEntry{"NtQueryInformationProcess", 2, [](Marshaler&) {}, 20});
    ASSERT_TRUE(registry.register_dll("ntdll.dll", std::move(exports)).is_ok());
    const GuestAddr base = modules.load_library("ntdll.dll");
    ASSERT_NE(base, 0u);

    for (int pass = 0; pass < 2; ++pass) {
        modules.materialize_synthetic_modules();
        for (const char* name :
             {"NtQueryObject", "NtClose", "NtQueryInformationProcess"}) {
            const GuestAddr entry = modules.get_proc_address(base, name);
            ASSERT_NE(entry, 0u);
            const u32 service = memory.read_u32(
                entry + (is_64bit ? 4 : 1)).value();
            ASSERT_LE(service, 0xFFFFu);
            const GuestAddr dispatch = registry.native_service_thunk(service);
            ASSERT_NE(dispatch, 0u) << name << " service=" << service;
            EXPECT_EQ(dispatch,
                      registry.get_thunk_address("ntdll.dll", name).value());
            EXPECT_TRUE(registry.resolve_handler(dispatch).is_ok());
        }
    }
}

// ntdll_service_layout_probe: every service is exported as NtXxx and ZwXxx at
// one stub; the stubs are contiguous at 32 bytes (x64) / 16 bytes (WoW64) in
// ascending number order; the number is the stub's rank; and the Nt* exports
// with no Zw* twin are not services. A guest that sorts the Zw* exports by
// address therefore derives the number the stub dispatches.
TEST(ModuleManagerSyntheticImageTest, NtdllServicesAreNumberedByRankInStubOrder) {
    for (const bool is_64bit : {true, false}) {
        SCOPED_TRACE(is_64bit ? "x64" : "x86");
        GuestMemory memory;
        capsule::Vfs vfs;
        ThunkRegistry registry(
            is_64bit ? 0x00007FFB00000000ull : 0x70000000ull);
        ModuleManager modules(registry);
        modules.attach_dynamic_loader(memory, vfs, is_64bit);
        DllExports exports;
        for (const char* name :
             {"NtQueryObject", "NtClose", "ZwOpenKey", "NtGetTickCount",
              "NtdllDefWindowProc_A", "RtlGetVersion"}) {
            exports.entries.push_back(
                ExportEntry{name, 0, [](Marshaler&) {}, 8});
        }
        add_ntdll_service_twins(exports);
        ASSERT_TRUE(
            registry.register_dll("ntdll.dll", std::move(exports)).is_ok());
        const GuestAddr base = modules.load_library("ntdll.dll");
        ASSERT_NE(base, 0u);
        modules.materialize_synthetic_modules();

        const u32 stride = is_64bit ? 32u : 16u;
        const u32 number_at = is_64bit ? 4u : 1u;
        const GuestAddr first = modules.get_proc_address(base, "ZwClose");
        ASSERT_NE(first, 0u);
        const char* services[] = {"Close", "OpenKey", "QueryObject"};
        for (u32 n = 0; n < 3; ++n) {
            const std::string nt = std::string("Nt") + services[n];
            const std::string zw = std::string("Zw") + services[n];
            SCOPED_TRACE(nt);
            const GuestAddr stub = modules.get_proc_address(base, zw.c_str());
            EXPECT_EQ(modules.get_proc_address(base, nt.c_str()), stub);
            EXPECT_EQ(stub, first + n * stride);
            EXPECT_EQ(memory.read_u32(stub + number_at).value(), n);
            EXPECT_EQ(registry.native_service_thunk(n),
                      registry.get_thunk_address("ntdll.dll", nt).value());
        }
        // Past the table, a number names no service.
        EXPECT_EQ(registry.native_service_thunk(3), 0u);

        for (const char* name :
             {"NtGetTickCount", "NtdllDefWindowProc_A", "RtlGetVersion"}) {
            SCOPED_TRACE(name);
            const GuestAddr entry = modules.get_proc_address(base, name);
            ASSERT_NE(entry, 0u);
            EXPECT_TRUE(entry < first || entry >= first + 3 * stride);
        }
        EXPECT_EQ(modules.get_proc_address(base, "ZwGetTickCount"), 0u);
    }
}

// The twin is the export ThunkRegistry keeps for the name (the last entry of
// a repeated one), under the other name, name-only.
TEST(NtdllSystemServicesTest, TwinsCopyTheKeptEntryOfEachService) {
    int called = 0;
    DllExports exports;
    exports.entries.push_back(
        ExportEntry{"NtOpenKey", 5, [&](Marshaler&) { called = 1; }, 12});
    exports.entries.push_back(
        ExportEntry{"NtOpenKey", 6, [&](Marshaler&) { called = 2; }, 12});
    exports.entries.push_back(
        ExportEntry{"ZwClose", 7, [&](Marshaler&) { called = 3; }, 4});
    exports.entries.push_back(ExportEntry{"NtQueryObject", 8, {}, 20});
    exports.entries.push_back(ExportEntry{"ZwQueryObject", 9, {}, 20});
    exports.entries.push_back(ExportEntry{"NtGetTickCount", 10, {}, 0});
    exports.entries.push_back(ExportEntry{"NtCurrentTeb", 11, {}, 0});
    exports.entries.push_back(ExportEntry{"NtdllDialogWndProc_W", 12, {}, 16});
    ExportEntry hidden{"NtHiddenService", 13, {}, 0};
    hidden.internal = true;
    exports.entries.push_back(std::move(hidden));

    add_ntdll_service_twins(exports);

    ASSERT_EQ(exports.entries.size(), 11u);
    const ExportEntry& zw_open_key = exports.entries[9];
    const ExportEntry& nt_close = exports.entries[10];
    EXPECT_EQ(zw_open_key.name, "ZwOpenKey");
    EXPECT_EQ(zw_open_key.ordinal, 0u);
    EXPECT_EQ(zw_open_key.stdcall_bytes, 12u);
    EXPECT_EQ(nt_close.name, "NtClose");
    EXPECT_EQ(nt_close.ordinal, 0u);
    EXPECT_EQ(nt_close.stdcall_bytes, 4u);
    GuestMemory memory;
    Marshaler m(memory);
    zw_open_key.handler(m);
    EXPECT_EQ(called, 2);
    nt_close.handler(m);
    EXPECT_EQ(called, 3);

    EXPECT_TRUE(is_ntdll_system_service("ZwAnything"));
    EXPECT_TRUE(is_ntdll_system_service("NtClose"));
    EXPECT_FALSE(is_ntdll_system_service("NtGetTickCount"));
    EXPECT_FALSE(is_ntdll_system_service("NtCurrentTeb"));
    EXPECT_FALSE(is_ntdll_system_service("NtdllDefWindowProc_A"));
    EXPECT_FALSE(is_ntdll_system_service("RtlGetVersion"));
    EXPECT_FALSE(is_ntdll_system_service("Nt"));
}
