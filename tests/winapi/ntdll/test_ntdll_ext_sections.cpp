// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_ntdll_ext_sections.cpp
/// @brief Unit tests for ntdll extended functions (WIN32-086): object-name, section query, map/unmap view

#include "ntdll_ext_test_fixture.hpp"

#include <cstring>
#include <vector>

TEST_F(NtdllExtTest, QueryObjectNamesFileMappingAsSectionAtNativeWidth) {
    constexpr GuestAddr DATA_BASE = 0x1000;
    constexpr GuestAddr INFO_ADDRESS = DATA_BASE + 0x100;
    constexpr GuestAddr RETURN_LENGTH_ADDRESS = DATA_BASE + 0x300;
    constexpr GuestAddr STACK_BASE = 0x80000;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        DATA_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
    HandleTable handles;
    FileMappingObject section{};
    const Handle section_handle =
        handles.allocate(HandleType::FileMapping, &section);
    mgr.set_handle_table(&handles);

    auto exports = make_ntdll_ext_exports(mgr);
    auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
                              [](const ExportEntry& candidate) {
                                  return candidate.name == "NtQueryObject";
                              });
    ASSERT_NE(entry, exports.entries.end());

    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Long;
    cpu.gpr.rsp = STACK_BASE + 0x800;
    cpu.gpr.rcx = section_handle;
    cpu.gpr.rdx = 2; // ObjectTypeInformation
    cpu.gpr.r8 = 0;
    cpu.gpr.r9 = 0;
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x28, RETURN_LENGTH_ADDRESS).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    entry->handler(marshaler);

    constexpr u32 REQUIRED = 104 + 16; // structure + u"Section" + NUL
    EXPECT_EQ(cpu.gpr.rax, nt_status::STATUS_INFO_LENGTH_MISMATCH);
    EXPECT_EQ(memory.read_u32(RETURN_LENGTH_ADDRESS).value(), REQUIRED);

    cpu.gpr.r8 = INFO_ADDRESS;
    cpu.gpr.r9 = REQUIRED;
    entry->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, nt_status::STATUS_SUCCESS);
    EXPECT_EQ(memory.read_u16(INFO_ADDRESS).value(), 14u);
    EXPECT_EQ(memory.read_u16(INFO_ADDRESS + 2).value(), 16u);
    EXPECT_EQ(memory.read_u64(INFO_ADDRESS + 8).value(),
              INFO_ADDRESS + 104);
    const std::u16string expected = u"Section";
    for (usize index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(memory.read_u16(INFO_ADDRESS + 104 + index * 2).value(),
                  static_cast<u16>(expected[index]));
    }
    EXPECT_EQ(memory.read_u16(INFO_ADDRESS + 104 + 14).value(), 0u);
}

TEST_F(NtdllExtTest, QueryObjectBasicInformationMapsFileGenericAccess) {
    constexpr GuestAddr DATA_BASE = 0x1000;
    constexpr GuestAddr INFO_ADDRESS = DATA_BASE + 0x100;
    constexpr GuestAddr RETURN_LENGTH_ADDRESS = DATA_BASE + 0x200;
    constexpr GuestAddr STACK_BASE = 0x80000;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        DATA_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
    HandleTable handles;
    const Handle file = handles.allocate(
        HandleType::File, nullptr, HandleFlags::Inherit, GENERIC_READ);
    mgr.set_handle_table(&handles);

    auto exports = make_ntdll_ext_exports(mgr);
    auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
                              [](const ExportEntry& candidate) {
                                  return candidate.name == "NtQueryObject";
                              });
    ASSERT_NE(entry, exports.entries.end());

    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Long;
    cpu.gpr.rsp = STACK_BASE + 0x800;
    cpu.gpr.rcx = file;
    cpu.gpr.rdx = 0; // ObjectBasicInformation
    cpu.gpr.r8 = INFO_ADDRESS;
    cpu.gpr.r9 = 56;
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x28, RETURN_LENGTH_ADDRESS).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    entry->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, nt_status::STATUS_SUCCESS);
    EXPECT_EQ(memory.read_u32(INFO_ADDRESS).value(), 0x2u); // OBJ_INHERIT
    EXPECT_EQ(memory.read_u32(INFO_ADDRESS + 4).value(), 0x00120089u);
    EXPECT_EQ(memory.read_u32(INFO_ADDRESS + 8).value(), 1u);
    EXPECT_EQ(memory.read_u32(INFO_ADDRESS + 12).value(), 1u);
    EXPECT_EQ(memory.read_u32(RETURN_LENGTH_ADDRESS).value(), 56u);
}

TEST_F(NtdllExtTest, QuerySectionReturnsTrackedBasicInformationAtNativeWidth) {
    constexpr GuestAddr DATA_BASE = 0x1000;
    constexpr GuestAddr INFO_ADDRESS = DATA_BASE + 0x100;
    constexpr GuestAddr RETURN_LENGTH_ADDRESS = DATA_BASE + 0x200;
    constexpr GuestAddr STACK_BASE = 0x80000;
    constexpr u64 SECTION_SIZE = 0x40000;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(DATA_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
    HandleTable handles;
    FileMappingObject section{};
    section.size = SECTION_SIZE;
    const Handle section_handle = handles.allocate(HandleType::FileMapping, &section);
    mgr.set_handle_table(&handles);

    auto exports = make_ntdll_ext_exports(mgr);
    auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
                              [](const ExportEntry& candidate) {
                                  return candidate.name == "NtQuerySection";
                              });
    ASSERT_NE(entry, exports.entries.end());

    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Long;
    cpu.gpr.rsp = STACK_BASE + 0x800;
    cpu.gpr.rcx = section_handle;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = INFO_ADDRESS;
    cpu.gpr.r9 = 24;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, RETURN_LENGTH_ADDRESS).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    entry->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, nt_status::STATUS_SUCCESS);
    EXPECT_EQ(memory.read_u64(INFO_ADDRESS).value(), 0u);
    EXPECT_EQ(memory.read_u32(INFO_ADDRESS + 8).value(), 0x08000000u);
    EXPECT_EQ(memory.read_u64(INFO_ADDRESS + 16).value(), SECTION_SIZE);
    EXPECT_EQ(memory.read_u32(RETURN_LENGTH_ADDRESS).value(), 24u);
}

TEST_F(NtdllExtTest, QuerySectionRequiresSectionQueryAccess) {
    constexpr GuestAddr DATA_BASE = 0x1000;
    constexpr GuestAddr INFO_ADDRESS = DATA_BASE + 0x100;
    constexpr GuestAddr STACK_BASE = 0x80000;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        DATA_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
    HandleTable handles;
    FileMappingObject section{};
    section.size = 0x40000;
    constexpr u32 FILE_MAP_READ = 0x00000004;
    const Handle section_handle = handles.allocate(
        HandleType::FileMapping, &section, 0, FILE_MAP_READ);
    mgr.set_handle_table(&handles);

    auto exports = make_ntdll_ext_exports(mgr);
    auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
                              [](const ExportEntry& candidate) {
                                  return candidate.name == "NtQuerySection";
                              });
    ASSERT_NE(entry, exports.entries.end());

    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Long;
    cpu.gpr.rsp = STACK_BASE + 0x800;
    cpu.gpr.rcx = section_handle;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = INFO_ADDRESS;
    cpu.gpr.r9 = 24;

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    entry->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, nt_status::STATUS_ACCESS_DENIED);
}

TEST_F(NtdllExtTest, MapAndUnmapViewOfSectionUseSharedSectionBackend) {
    constexpr GuestAddr DATA_BASE = 0x10000;
    constexpr GuestAddr BASE_ADDRESS_PTR = DATA_BASE + 0x100;
    constexpr GuestAddr VIEW_SIZE_PTR = DATA_BASE + 0x108;
    constexpr GuestAddr STACK_BASE = 0x80000;
    constexpr u64 SECTION_SIZE = 0x3000;
    constexpr u32 PAGE_READWRITE = 0x04;
    constexpr u32 VIEW_UNMAP = 2;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        DATA_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
    HandleTable handles;
    FileMappingManager file_mapping(handles, memory);
    const Handle section_handle = file_mapping.create_file_mapping(
        FILE_MAPPING_INVALID_FILE, PAGE_READWRITE, SECTION_SIZE, "");
    ASSERT_NE(section_handle, NULL_HANDLE);
    mgr.set_handle_table(&handles);
    mgr.set_file_mapping_manager(&file_mapping);

    auto exports = make_ntdll_ext_exports(mgr);
    auto map_entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "NtMapViewOfSection";
        });
    auto unmap_entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "NtUnmapViewOfSection";
        });
    ASSERT_NE(map_entry, exports.entries.end());
    ASSERT_NE(unmap_entry, exports.entries.end());

    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Long;
    cpu.gpr.rsp = STACK_BASE + 0x400;
    cpu.gpr.rcx = section_handle;
    cpu.gpr.rdx = 0xFFFFFFFFFFFFFFFFULL;
    cpu.gpr.r8 = BASE_ADDRESS_PTR;
    cpu.gpr.r9 = 0;
    ASSERT_TRUE(memory.write_u64(BASE_ADDRESS_PTR, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(VIEW_SIZE_PTR, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x38, VIEW_SIZE_PTR).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x40, VIEW_UNMAP).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x48, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x50, PAGE_READWRITE).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    map_entry->handler(marshaler);

    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), nt_status::STATUS_SUCCESS);
    const GuestAddr mapped_base = memory.read_u64(BASE_ADDRESS_PTR).value();
    EXPECT_NE(mapped_base, 0u);
    EXPECT_EQ(memory.read_u64(VIEW_SIZE_PTR).value(), SECTION_SIZE);
    ASSERT_TRUE(memory.write_u32(mapped_base, 0xA5A55A5Au).is_ok());

    cpu.gpr.rcx = 0xFFFFFFFFFFFFFFFFULL;
    cpu.gpr.rdx = mapped_base;
    unmap_entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), nt_status::STATUS_SUCCESS);
    EXPECT_FALSE(memory.is_mapped(mapped_base, 1));
}

TEST_F(NtdllExtTest, MapViewOfSectionRejectsRemoteProcessHonestly) {
    constexpr GuestAddr DATA_BASE = 0x10000;
    constexpr GuestAddr STACK_BASE = 0x80000;
    constexpr u32 STATUS_NOT_SUPPORTED = 0xC00000BBu;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        DATA_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
    HandleTable handles;
    FileMappingManager file_mapping(handles, memory);
    mgr.set_handle_table(&handles);
    mgr.set_file_mapping_manager(&file_mapping);

    auto exports = make_ntdll_ext_exports(mgr);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "NtMapViewOfSection";
        });
    ASSERT_NE(entry, exports.entries.end());

    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Long;
    cpu.gpr.rsp = STACK_BASE + 0x400;
    cpu.gpr.rcx = 1;
    cpu.gpr.rdx = 0x1234;
    cpu.gpr.r8 = DATA_BASE;
    cpu.gpr.r9 = 0;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), STATUS_NOT_SUPPORTED);
}

TEST_F(NtdllExtTest, ObjectNameAndCreateSectionSupportRendererSignedPolicy) {
    // Chromium SignedPolicy::GenerateRules: CreateFile(FILE_EXECUTE) →
    // NtQueryObject(ObjectNameInformation) → NtCreateSection(SEC_IMAGE).
    char path_template[] = "/tmp/vitreo_ntsection_XXXXXX";
    char* root = ::mkdtemp(path_template);
    ASSERT_NE(root, nullptr);
    std::filesystem::create_directories(std::string(root) + "/Program Files/App");
    const std::string host_dll =
        std::string(root) + "/Program Files/App/payload.dll";
    {
        // A minimal x64 DLL: headers plus one code section. SEC_IMAGE needs a
        // well-formed image (an "MZ" stub alone is STATUS_INVALID_IMAGE_FORMAT).
        std::vector<char> pe(0x400, 0);
        auto put16 = [&](usize o, u16 v) { std::memcpy(&pe[o], &v, 2); };
        auto put32 = [&](usize o, u32 v) { std::memcpy(&pe[o], &v, 4); };
        auto put64 = [&](usize o, u64 v) { std::memcpy(&pe[o], &v, 8); };
        put16(0, 0x5A4D);           // MZ
        put32(0x3C, 0x80);          // e_lfanew
        put32(0x80, 0x00004550);    // PE\0\0
        put16(0x84, 0x8664);        // AMD64
        put16(0x86, 1);             // NumberOfSections
        put16(0x94, 0xF0);          // SizeOfOptionalHeader
        put16(0x96, 0x2022);        // EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE | DLL
        constexpr usize opt = 0x98;
        put16(opt, 0x20B);          // PE32+
        put64(opt + 24, 0x180000000ull);
        put32(opt + 32, 0x1000);    // SectionAlignment
        put32(opt + 36, 0x200);     // FileAlignment
        put16(opt + 40, 6);
        put16(opt + 48, 6);
        put32(opt + 56, 0x2000);    // SizeOfImage
        put32(opt + 60, 0x200);     // SizeOfHeaders
        put16(opt + 68, 2);         // IMAGE_SUBSYSTEM_WINDOWS_GUI
        put32(opt + 108, 16);       // NumberOfRvaAndSizes
        constexpr usize sh = opt + 0xF0;
        std::memcpy(&pe[sh], ".text", 5);
        put32(sh + 8, 0x200);       // VirtualSize
        put32(sh + 12, 0x1000);     // VirtualAddress
        put32(sh + 16, 0x200);      // SizeOfRawData
        put32(sh + 20, 0x200);      // PointerToRawData
        put32(sh + 36, 0x60000020u); // CODE | EXECUTE | READ
        pe[0x200] = static_cast<char>(0xC3);  // ret
        std::ofstream out(host_dll, std::ios::binary);
        out.write(pe.data(), static_cast<std::streamsize>(pe.size()));
    }

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
    HandleTable handles;
    FileManager file_manager(vfs, handles);
    GuestMemory memory;
    constexpr GuestAddr DATA_BASE = 0x10000;
    constexpr GuestAddr STACK_BASE = 0x80000;
    ASSERT_TRUE(memory.map_region(
        DATA_BASE, 0x2000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
    FileMappingManager mapping(handles, memory);
    mapping.set_file_manager(&file_manager);

    mgr.set_handle_table(&handles);
    mgr.set_file_manager(&file_manager);
    mgr.set_file_mapping_manager(&mapping);

    constexpr u32 FILE_EXECUTE = 0x0020;
    constexpr u32 FILE_SHARE_READ = 0x00000001;
    constexpr u32 FILE_SHARE_DELETE = 0x00000004;
    constexpr u32 OPEN_EXISTING = 3;
    const Handle file = file_manager.create_file(
        "C:\\Program Files\\App\\payload.dll", FILE_EXECUTE,
        FILE_SHARE_READ | FILE_SHARE_DELETE, OPEN_EXISTING, 0);
    ASSERT_NE(file, INVALID_HANDLE);

    auto exports = make_ntdll_ext_exports(mgr);
    auto query_entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "NtQueryObject"; });
    auto create_entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "NtCreateSection"; });
    ASSERT_NE(query_entry, exports.entries.end());
    ASSERT_NE(create_entry, exports.entries.end());

    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Long;
    cpu.gpr.rsp = STACK_BASE + 0x400;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr info = DATA_BASE;
    constexpr GuestAddr ret_len = DATA_BASE + 0x800;
    cpu.gpr.rcx = file;
    cpu.gpr.rdx = 1; // ObjectNameInformation
    cpu.gpr.r8 = info;
    cpu.gpr.r9 = 0x800;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, ret_len).is_ok());
    query_entry->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), nt_status::STATUS_SUCCESS);
    const u16 name_bytes = memory.read_u16(info).value();
    ASSERT_GT(name_bytes, 0u);
    const GuestAddr name_ptr = memory.read_u64(info + 8).value();
    ASSERT_EQ(name_ptr, info + 16);
    EXPECT_EQ(memory.read_u16(name_ptr).value(), static_cast<u16>('\\'));
    EXPECT_EQ(memory.read_u16(name_ptr + 2).value(), static_cast<u16>('?'));

    constexpr GuestAddr section_out = DATA_BASE + 0x900;
    constexpr GuestAddr object_attr = DATA_BASE + 0xA00;
    constexpr u32 SECTION_QUERY = 0x0001;
    constexpr u32 SECTION_MAP_READ = 0x0004;
    constexpr u32 SECTION_MAP_WRITE = 0x0002;
    constexpr u32 SECTION_MAP_EXECUTE = 0x0008;
    constexpr u32 PAGE_EXECUTE = 0x10;
    constexpr u32 SEC_IMAGE = 0x01000000;
    constexpr u32 OBJ_INHERIT = 0x00000002;
    // x64 OBJECT_ATTRIBUTES: Attributes ULONG at offset 24.
    for (u32 i = 0; i < 48; ++i) {
        ASSERT_TRUE(memory.write_u8(object_attr + i, 0).is_ok());
    }
    ASSERT_TRUE(memory.write_u32(object_attr, 48).is_ok()); // Length
    ASSERT_TRUE(memory.write_u32(object_attr + 24, OBJ_INHERIT).is_ok());
    cpu.gpr.rcx = section_out;
    cpu.gpr.rdx = SECTION_QUERY | SECTION_MAP_READ | SECTION_MAP_WRITE |
                  SECTION_MAP_EXECUTE;
    cpu.gpr.r8 = object_attr;
    cpu.gpr.r9 = 0; // MaximumSize
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, PAGE_EXECUTE).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, SEC_IMAGE).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x38, file).is_ok());
    create_entry->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), nt_status::STATUS_SUCCESS);
    const Handle section = static_cast<Handle>(memory.read_u64(section_out).value());
    EXPECT_NE(section, NULL_HANDLE);
    EXPECT_EQ(handles.get_type(section), HandleType::FileMapping);
    EXPECT_NE(handles.get_flags(section) & HandleFlags::Inherit, 0u);

    mgr.set_file_mapping_manager(nullptr);
    mgr.set_file_manager(nullptr);
    mgr.set_handle_table(nullptr);
    std::filesystem::remove_all(root);
}
