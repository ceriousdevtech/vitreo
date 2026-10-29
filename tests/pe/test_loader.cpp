// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// PE Loader integration tests (PE-005)

#include <gtest/gtest.h>
#include <core/pe/loader/pe_loader.hpp>
#include <core/pe/parser/pe_parser.hpp>
#include <core/pe/parser/pe_structures.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>
#include <cstring>
#include <vector>

using namespace vitreo;
using namespace vitreo::pe;

// ============================================================================
// Test Fixture
// ============================================================================

class PeLoaderTest : public ::testing::Test {
protected:
    GuestMemory memory;
    winapi::ThunkRegistry thunks{0xF0000000, 16};

    /// Build a minimal valid PE64 (PE32+) binary in a vector
    static std::vector<u8> build_minimal_pe64() {
        // Layout:
        //   0x000 - DOS header (64 bytes), padding to 0x80
        //   0x080 - PE signature (4) + COFF header (20) + OptionalHeader64 (240)
        //   0x080 + 4 + 20 + 240 = 0x080 + 264 = 0x188
        //   0x188 - Section header: .text (40 bytes)
        //   Pad to 0x200 for headers
        //   0x200 - .text raw data (0x200 bytes)
        //   Total: 0x400 bytes

        constexpr u32 pe_offset = 0x80;
        constexpr u32 headers_size = 0x200;
        constexpr u32 text_file_offset = 0x200;
        constexpr u32 text_raw_size = 0x200;
        constexpr u32 total_size = text_file_offset + text_raw_size;

        std::vector<u8> data(total_size, 0);

        // --- DOS Header ---
        auto* dos = reinterpret_cast<DosHeader*>(data.data());
        dos->e_magic = DOS_MAGIC; // 'MZ'
        dos->e_lfanew = pe_offset;

        // --- PE Signature ---
        u32 pe_sig = PE_SIGNATURE;
        std::memcpy(data.data() + pe_offset, &pe_sig, 4);

        // --- COFF Header ---
        constexpr u32 coff_offset = pe_offset + 4;
        auto* coff = reinterpret_cast<CoffHeader*>(data.data() + coff_offset);
        coff->Machine = static_cast<u16>(MachineType::AMD64);
        coff->NumberOfSections = 1;
        coff->SizeOfOptionalHeader = 240; // sizeof(OptionalHeader64)
        coff->Characteristics = CoffCharacteristics::EXECUTABLE_IMAGE
                              | CoffCharacteristics::LARGE_ADDRESS_AWARE;

        // --- Optional Header (PE32+) ---
        constexpr u32 opt_offset = coff_offset + sizeof(CoffHeader);
        auto* opt = reinterpret_cast<OptionalHeader64*>(data.data() + opt_offset);
        opt->Magic = PE32PLUS_MAGIC;  // 0x20b
        opt->AddressOfEntryPoint = 0x1000;
        opt->ImageBase = 0x400000;
        opt->SectionAlignment = 0x1000;
        opt->FileAlignment = 0x200;
        opt->MajorOperatingSystemVersion = 6;
        opt->MajorSubsystemVersion = 6;
        opt->SizeOfImage = 0x2000;
        opt->SizeOfHeaders = headers_size;
        opt->Subsystem = static_cast<u16>(Subsystem::WindowsCui);
        opt->NumberOfRvaAndSizes = DIR_COUNT;

        // --- Section Header (.text) ---
        constexpr u32 section_offset = opt_offset + 240;
        auto* sec = reinterpret_cast<SectionHeader*>(data.data() + section_offset);
        std::memcpy(sec->Name, ".text\0\0\0", 8);
        sec->VirtualSize = 0x100;
        sec->VirtualAddress = 0x1000;
        sec->SizeOfRawData = text_raw_size;
        sec->PointerToRawData = text_file_offset;
        sec->Characteristics = SCN_CNT_CODE | SCN_MEM_EXECUTE | SCN_MEM_READ;

        // --- .text section data: RET instruction ---
        data[text_file_offset] = 0xC3; // RET

        return data;
    }

    /// Minimal PE64 whose import table names testdll.dll!ImportedFunction.
    static std::vector<u8> build_pe64_importing_testdll() {
        auto pe_data = build_minimal_pe64();
        auto* optional = reinterpret_cast<OptionalHeader64*>(
            pe_data.data() + 0x80 + sizeof(u32) + sizeof(CoffHeader));
        optional->DataDirectory[DIR_IMPORT].VirtualAddress = 0x1000;
        optional->DataDirectory[DIR_IMPORT].Size =
            sizeof(ImportDescriptor) * 2;

        ImportDescriptor descriptor{};
        descriptor.OriginalFirstThunk = 0x1060;
        descriptor.Name = 0x1040;
        descriptor.FirstThunk = 0x10A0;
        std::memcpy(pe_data.data() + 0x200, &descriptor, sizeof(descriptor));
        std::memcpy(pe_data.data() + 0x240, "testdll.dll", 12);
        constexpr u64 name_thunk = 0x1080;
        std::memcpy(pe_data.data() + 0x260, &name_thunk, sizeof(name_thunk));
        std::memcpy(pe_data.data() + 0x2A0, &name_thunk, sizeof(name_thunk));
        constexpr u16 hint = 0;
        std::memcpy(pe_data.data() + 0x280, &hint, sizeof(hint));
        std::memcpy(pe_data.data() + 0x282, "ImportedFunction", 17);
        return pe_data;
    }
};

// ============================================================================
// Tests
// ============================================================================

TEST_F(PeLoaderTest, LoadValidPe64) {
    auto pe_data = build_minimal_pe64();
    PeLoader loader(memory, thunks);

    auto result = loader.load(pe_data);
    ASSERT_TRUE(result.is_ok()) << "load() should succeed for valid PE64";

    auto& img = result.value();
    EXPECT_NE(img.image_base, 0u);
    EXPECT_NE(img.entry_point, 0u);
}

TEST_F(PeLoaderTest, EntryPointRelocated) {
    auto pe_data = build_minimal_pe64();
    PeLoader loader(memory, thunks);

    auto result = loader.load(pe_data);
    ASSERT_TRUE(result.is_ok());

    auto& img = result.value();
    // entry_point should be base + entry_rva (0x1000)
    EXPECT_EQ(img.entry_point, img.image_base + 0x1000);
}

TEST_F(PeLoaderTest, SectionsPopulated) {
    auto pe_data = build_minimal_pe64();
    PeLoader loader(memory, thunks);

    auto result = loader.load(pe_data);
    ASSERT_TRUE(result.is_ok());

    auto& img = result.value();
    EXPECT_FALSE(img.sections.empty());
}

TEST_F(PeLoaderTest, Bitness64) {
    auto pe_data = build_minimal_pe64();
    PeLoader loader(memory, thunks);

    auto result = loader.load(pe_data);
    ASSERT_TRUE(result.is_ok());

    EXPECT_EQ(result.value().bitness, 64u);
}

TEST_F(PeLoaderTest, InvalidPeReturnsError) {
    std::vector<u8> empty;
    PeLoader loader(memory, thunks);

    auto result = loader.load(empty);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), Error::InvalidFormat);
}

TEST_F(PeLoaderTest, InvalidMagicReturnsError) {
    std::vector<u8> garbage(512, 0xAB);
    PeLoader loader(memory, thunks);

    auto result = loader.load(garbage);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), Error::InvalidFormat);
}

TEST_F(PeLoaderTest, NoRelocAtPreferredBase) {
    auto pe_data = build_minimal_pe64();
    PeLoader loader(memory, thunks);

    // Load at the PE's own preferred ImageBase (0x400000)
    auto result = loader.load(pe_data);
    ASSERT_TRUE(result.is_ok())
        << "Loading at PE's preferred base should succeed";
    EXPECT_EQ(result.value().image_base, 0x400000u);
}

TEST_F(PeLoaderTest, LoadTwiceDifferentBase) {
    auto pe_data = build_minimal_pe64();

    PeLoader loader(memory, thunks);

    // First load at default base
    GuestAddr base1 = 0x400000;
    auto r1 = loader.load(pe_data, base1);
    ASSERT_TRUE(r1.is_ok());

    // Second load at a different base (non-overlapping)
    GuestAddr base2 = 0x800000;
    auto r2 = loader.load(pe_data, base2);
    ASSERT_TRUE(r2.is_ok());

    EXPECT_NE(r1.value().image_base, r2.value().image_base);
}

TEST_F(PeLoaderTest, EntryPointNonZero) {
    auto pe_data = build_minimal_pe64();
    PeLoader loader(memory, thunks);

    auto result = loader.load(pe_data);
    ASSERT_TRUE(result.is_ok());

    EXPECT_NE(result.value().entry_point, 0u);
}

TEST_F(PeLoaderTest, ImageEndGreaterThanBase) {
    auto pe_data = build_minimal_pe64();
    PeLoader loader(memory, thunks);

    auto result = loader.load(pe_data);
    ASSERT_TRUE(result.is_ok());

    auto& img = result.value();
    EXPECT_GT(img.image_end, img.image_base);
    EXPECT_EQ(img.image_end, img.image_base + 0x2000);
}

TEST_F(PeLoaderTest, PreImportCallbackRunsBeforeIatResolution) {
    auto pe_data = build_pe64_importing_testdll();
    constexpr u64 name_thunk = 0x1080;

    PeLoader loader(memory, thunks);
    int sequence = 0;
    constexpr GuestAddr resolved_address = 0x123456789ABCDEF0ull;
    auto result = loader.load(
        pe_data, 0,
        [&](std::string_view dll_name, std::string_view function_name, u16) {
            EXPECT_EQ(sequence, 1);
            EXPECT_EQ(dll_name, "testdll.dll");
            EXPECT_EQ(function_name, "ImportedFunction");
            sequence = 2;
            return resolved_address;
        },
        [&](GuestAddr image_base, usize image_size, bool is_64bit) {
            EXPECT_EQ(sequence, 0);
            EXPECT_EQ(image_size, 0x2000u);
            EXPECT_TRUE(is_64bit);
            EXPECT_EQ(memory.read_u64(image_base + 0x10A0).value(), name_thunk);
            sequence = 1;
        });

    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(sequence, 2);
    EXPECT_EQ(memory.read_u64(result.value().image_base + 0x10A0).value(),
              resolved_address);
}

TEST_F(PeLoaderTest, MissingVendorDllFailsLoadWithoutStub) {
    // Learn: Load-Time Dynamic Linking — if the system cannot locate a
    // required DLL, it terminates the process. An unregistered vendor DLL
    // must not be replaced with a missing-export stub.
    auto pe_data = build_pe64_importing_testdll();
    PeLoader loader(memory, thunks);

    auto result = loader.load(pe_data);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), Error::NotFound);
}

TEST_F(PeLoaderTest, RegisteredHostDllMissingFunctionStillStubs) {
    winapi::DllExports exports;
    exports.entries.push_back(winapi::ExportEntry{
        "SomeOtherExport", 1, [](winapi::Marshaler&) {}});
    ASSERT_TRUE(thunks.register_dll("testdll.dll", std::move(exports)).is_ok());

    auto pe_data = build_pe64_importing_testdll();
    PeLoader loader(memory, thunks);
    auto result = loader.load(pe_data);
    ASSERT_TRUE(result.is_ok());
    EXPECT_NE(memory.read_u64(result.value().image_base + 0x10A0).value(), 0u);
}

TEST_F(PeLoaderTest, ApiSetImportResolvesThroughMappedHostDll) {
    winapi::DllExports exports;
    exports.entries.push_back(winapi::ExportEntry{
        "ImportedFunction", 1, [](winapi::Marshaler&) {}});
    ASSERT_TRUE(thunks.register_dll("ucrtbase.dll", std::move(exports)).is_ok());

    auto pe_data = build_pe64_importing_testdll();
    auto* descriptor = reinterpret_cast<ImportDescriptor*>(pe_data.data() + 0x200);
    descriptor->OriginalFirstThunk = 0x1070;
    descriptor->Name = 0x1040;
    descriptor->FirstThunk = 0x10C0;
    constexpr char api_set[] = "api-ms-win-crt-runtime-l1-1-0.dll";
    std::memcpy(pe_data.data() + 0x240, api_set, sizeof(api_set));
    constexpr u64 name_thunk = 0x1080;
    std::memcpy(pe_data.data() + 0x270, &name_thunk, sizeof(name_thunk));
    std::memcpy(pe_data.data() + 0x2C0, &name_thunk, sizeof(name_thunk));

    PeLoader loader(memory, thunks);
    auto result = loader.load(pe_data);
    ASSERT_TRUE(result.is_ok());
    const u64 resolved = memory.read_u64(
        result.value().image_base + 0x10C0).value();
    EXPECT_EQ(resolved,
              thunks.get_thunk_address("ucrtbase.dll", "ImportedFunction")
                  .value());
}
