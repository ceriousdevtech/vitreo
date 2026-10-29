// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Delay import processor unit tests

#include <gtest/gtest.h>
#include <core/pe/imports/delay_import.hpp>
#include <core/pe/parser/pe_parser.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <cstring>

using namespace vitreo;
using namespace vitreo::pe;

// ============================================================================
// Test Fixture
// ============================================================================

class DelayImportTest : public ::testing::Test {
protected:
    GuestMemory memory;
    // Thunk region at 0x70000000
    winapi::ThunkRegistry thunks{0x70000000};

    static constexpr u64 BASE_ADDRESS = 0x00400000;

    void SetUp() override {
        // Map guest memory for IAT / hmod writes
        // Region covers base_address + sections
        auto result = memory.map_region(BASE_ADDRESS, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(result.is_ok());
    }

    // ----------------------------------------------------------------
    // PE builder helpers
    // ----------------------------------------------------------------

    /// Build a minimal PE32 skeleton with 3 sections (.text, .didata, .data)
    /// Caller populates delay import data after the call.
    std::vector<u8> create_base_pe(bool has_delay_dir, u32 delay_dir_size = sizeof(ImgDelayDescr) * 2) {
        std::vector<u8> data;

        // DOS Header
        DosHeader dos{};
        dos.e_magic = DOS_MAGIC;
        dos.e_lfanew = sizeof(DosHeader);
        data.resize(sizeof(DosHeader));
        std::memcpy(data.data(), &dos, sizeof(DosHeader));

        // PE Signature
        u32 pe_sig = PE_SIGNATURE;
        auto pe_off = data.size();
        data.resize(data.size() + 4);
        std::memcpy(data.data() + pe_off, &pe_sig, 4);

        // COFF Header
        CoffHeader coff{};
        coff.Machine = static_cast<u16>(MachineType::I386);
        coff.NumberOfSections = 3;
        coff.SizeOfOptionalHeader = sizeof(OptionalHeader32);
        coff.Characteristics = CoffCharacteristics::EXECUTABLE_IMAGE;
        auto coff_off = data.size();
        data.resize(data.size() + sizeof(CoffHeader));
        std::memcpy(data.data() + coff_off, &coff, sizeof(CoffHeader));

        // Optional Header (PE32)
        OptionalHeader32 opt{};
        opt.Magic = PE32_MAGIC;
        opt.AddressOfEntryPoint = 0x1000;
        opt.ImageBase = static_cast<u32>(BASE_ADDRESS);
        opt.SectionAlignment = 0x1000;
        opt.FileAlignment = 0x200;
        opt.SizeOfImage = 0x5000;
        opt.SizeOfHeaders = 0x400;
        opt.Subsystem = static_cast<u16>(Subsystem::WindowsCui);
        opt.NumberOfRvaAndSizes = 16;

        if (has_delay_dir) {
            opt.DataDirectory[DIR_DELAY_IMPORT].VirtualAddress = 0x2000;
            opt.DataDirectory[DIR_DELAY_IMPORT].Size = delay_dir_size;
        }

        auto opt_off = data.size();
        data.resize(data.size() + sizeof(OptionalHeader32));
        std::memcpy(data.data() + opt_off, &opt, sizeof(OptionalHeader32));

        // Section 1: .text
        SectionHeader sec1{};
        std::memcpy(sec1.Name, ".text\0\0\0", 8);
        sec1.VirtualSize = 0x100;
        sec1.VirtualAddress = 0x1000;
        sec1.SizeOfRawData = 0x200;
        sec1.PointerToRawData = 0x400;
        sec1.Characteristics = SCN_CNT_CODE | SCN_MEM_EXECUTE | SCN_MEM_READ;
        auto s1_off = data.size();
        data.resize(data.size() + sizeof(SectionHeader));
        std::memcpy(data.data() + s1_off, &sec1, sizeof(SectionHeader));

        // Section 2: .didata (delay imports)  RVA 0x2000  file 0x600
        SectionHeader sec2{};
        std::memcpy(sec2.Name, ".didata\0", 8);
        sec2.VirtualSize = 0x400;
        sec2.VirtualAddress = 0x2000;
        sec2.SizeOfRawData = 0x400;
        sec2.PointerToRawData = 0x600;
        sec2.Characteristics = SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ | SCN_MEM_WRITE;
        auto s2_off = data.size();
        data.resize(data.size() + sizeof(SectionHeader));
        std::memcpy(data.data() + s2_off, &sec2, sizeof(SectionHeader));

        // Section 3: .data (IAT / hmod)  RVA 0x3000  file 0xA00
        SectionHeader sec3{};
        std::memcpy(sec3.Name, ".data\0\0\0", 8);
        sec3.VirtualSize = 0x200;
        sec3.VirtualAddress = 0x3000;
        sec3.SizeOfRawData = 0x200;
        sec3.PointerToRawData = 0xA00;
        sec3.Characteristics = SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ | SCN_MEM_WRITE;
        auto s3_off = data.size();
        data.resize(data.size() + sizeof(SectionHeader));
        std::memcpy(data.data() + s3_off, &sec3, sizeof(SectionHeader));

        // Pad to 0x400 (headers end)
        data.resize(0x400, 0);
        // .text data
        data.resize(0x600, 0);
        // .didata data
        data.resize(0xA00, 0);
        // .data section
        data.resize(0xC00, 0);

        return data;
    }

    /// Put a single delay descriptor + terminator + DLL name + INT + HintName
    /// into \p data.  DLL name = dll, one named import = func_name.
    /// IAT at RVA 0x3000, hmod at RVA 0x3080, INT at RVA 0x2080, name table at 0x20C0.
    void fill_single_named_import(std::vector<u8>& data,
                                  const char* dll,
                                  const char* func_name,
                                  u32 grAttrs = 1) {
        // Descriptor at file offset 0x600 (RVA 0x2000)
        ImgDelayDescr desc{};
        desc.grAttrs = grAttrs;

        u32 dll_name_rva = 0x2040;
        u32 int_rva      = 0x2080;
        u32 iat_rva      = 0x3000;
        u32 hmod_rva     = 0x3080;
        u32 hint_name_rva = 0x20C0;

        if (grAttrs == 0) {
            // VA-based: store image_base + RVA
            desc.rvaDLLName = static_cast<u32>(BASE_ADDRESS) + dll_name_rva;
            desc.rvaINT     = static_cast<u32>(BASE_ADDRESS) + int_rva;
            desc.rvaIAT     = static_cast<u32>(BASE_ADDRESS) + iat_rva;
            desc.rvaHmod    = static_cast<u32>(BASE_ADDRESS) + hmod_rva;
        } else {
            desc.rvaDLLName = dll_name_rva;
            desc.rvaINT     = int_rva;
            desc.rvaIAT     = iat_rva;
            desc.rvaHmod    = hmod_rva;
        }

        std::memcpy(data.data() + 0x600, &desc, sizeof(desc));

        // Null terminator descriptor
        ImgDelayDescr null_desc{};
        std::memcpy(data.data() + 0x600 + sizeof(ImgDelayDescr), &null_desc, sizeof(null_desc));

        // DLL name at file offset 0x640 (RVA 0x2040)
        std::memcpy(data.data() + 0x640, dll, std::strlen(dll) + 1);

        // INT at file offset 0x680 (RVA 0x2080): one thunk pointing to hint/name + null
        u32 thunk_val = hint_name_rva;
        std::memcpy(data.data() + 0x680, &thunk_val, sizeof(thunk_val));
        u32 null_thunk = 0;
        std::memcpy(data.data() + 0x684, &null_thunk, sizeof(null_thunk));

        // Hint/Name at file offset 0x6C0 (RVA 0x20C0): hint(2) + name
        u16 hint = 0;
        std::memcpy(data.data() + 0x6C0, &hint, sizeof(hint));
        std::memcpy(data.data() + 0x6C2, func_name, std::strlen(func_name) + 1);
    }

    /// Register a DLL with named functions in the thunk registry
    void register_dll(const char* dll_name, std::initializer_list<const char*> funcs) {
        winapi::DllExports exports;
        u16 ordinal = 1;
        for (auto fn : funcs) {
            winapi::ExportEntry e;
            e.name = fn;
            e.ordinal = ordinal++;
            e.handler = [](winapi::Marshaler&) {};
            exports.entries.push_back(std::move(e));
        }
        auto r = thunks.register_dll(dll_name, std::move(exports));
        ASSERT_TRUE(r.is_ok());
    }
};

// ============================================================================
// Tests
// ============================================================================

// 1. PE without delay imports returns NoDelayImports
TEST_F(DelayImportTest, NoDelayImportsReturnsError) {
    auto pe_data = create_base_pe(/*has_delay_dir=*/false);
    auto pe_result = parse_pe(pe_data);
    ASSERT_TRUE(pe_result.is_ok());

    DelayImportProcessor proc(memory, thunks);
    EXPECT_FALSE(DelayImportProcessor::has_delay_imports(pe_result.value()));

    auto result = proc.parse_delay_imports(pe_data, pe_result.value(), BASE_ADDRESS);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), DelayImportError::NoDelayImports);
}

// 2. Single DLL with one named import parses correctly
TEST_F(DelayImportTest, SingleDllOneNamedImport) {
    auto pe_data = create_base_pe(true);
    fill_single_named_import(pe_data, "ADVAPI32.dll", "RegOpenKeyExW");

    auto pe_result = parse_pe(pe_data);
    ASSERT_TRUE(pe_result.is_ok());
    ASSERT_TRUE(DelayImportProcessor::has_delay_imports(pe_result.value()));

    DelayImportProcessor proc(memory, thunks);
    auto result = proc.parse_delay_imports(pe_data, pe_result.value(), BASE_ADDRESS);
    ASSERT_TRUE(result.is_ok());

    auto& modules = result.value();
    ASSERT_EQ(modules.size(), 1u);
    EXPECT_EQ(modules[0].dll_name, "ADVAPI32.dll");
    ASSERT_EQ(modules[0].entries.size(), 1u);
    EXPECT_EQ(modules[0].entries[0].function_name, "RegOpenKeyExW");
    EXPECT_FALSE(modules[0].entries[0].is_ordinal);
}

// 3. Single DLL with multiple named imports
TEST_F(DelayImportTest, SingleDllMultipleImports) {
    auto pe_data = create_base_pe(true);

    // Descriptor
    ImgDelayDescr desc{};
    desc.grAttrs = 1;
    desc.rvaDLLName = 0x2040;
    desc.rvaINT     = 0x2080;
    desc.rvaIAT     = 0x3000;
    desc.rvaHmod    = 0x3080;
    std::memcpy(pe_data.data() + 0x600, &desc, sizeof(desc));

    // Null terminator
    ImgDelayDescr null_desc{};
    std::memcpy(pe_data.data() + 0x600 + sizeof(ImgDelayDescr), &null_desc, sizeof(null_desc));

    // DLL name
    const char* dll = "USER32.dll";
    std::memcpy(pe_data.data() + 0x640, dll, std::strlen(dll) + 1);

    // INT: 3 thunks pointing to hint/names + null
    u32 hn_rva1 = 0x20C0;
    u32 hn_rva2 = 0x20E0;
    u32 hn_rva3 = 0x2100;
    u32 null_t  = 0;
    std::memcpy(pe_data.data() + 0x680, &hn_rva1, 4);
    std::memcpy(pe_data.data() + 0x684, &hn_rva2, 4);
    std::memcpy(pe_data.data() + 0x688, &hn_rva3, 4);
    std::memcpy(pe_data.data() + 0x68C, &null_t, 4);

    // Hint/Name entries
    u16 hint = 0;
    std::memcpy(pe_data.data() + 0x6C0, &hint, 2);
    std::memcpy(pe_data.data() + 0x6C2, "MessageBoxA", 12);

    std::memcpy(pe_data.data() + 0x6E0, &hint, 2);
    std::memcpy(pe_data.data() + 0x6E2, "SendMessageW", 13);

    std::memcpy(pe_data.data() + 0x700, &hint, 2);
    std::memcpy(pe_data.data() + 0x702, "PostMessageW", 13);

    auto pe_result = parse_pe(pe_data);
    ASSERT_TRUE(pe_result.is_ok());

    DelayImportProcessor proc(memory, thunks);
    auto result = proc.parse_delay_imports(pe_data, pe_result.value(), BASE_ADDRESS);
    ASSERT_TRUE(result.is_ok());

    auto& modules = result.value();
    ASSERT_EQ(modules.size(), 1u);
    ASSERT_EQ(modules[0].entries.size(), 3u);
    EXPECT_EQ(modules[0].entries[0].function_name, "MessageBoxA");
    EXPECT_EQ(modules[0].entries[1].function_name, "SendMessageW");
    EXPECT_EQ(modules[0].entries[2].function_name, "PostMessageW");
}

// 4. Ordinal import resolves
TEST_F(DelayImportTest, OrdinalImportResolves) {
    auto pe_data = create_base_pe(true);

    ImgDelayDescr desc{};
    desc.grAttrs = 1;
    desc.rvaDLLName = 0x2040;
    desc.rvaINT     = 0x2080;
    desc.rvaIAT     = 0x3000;
    desc.rvaHmod    = 0x3080;
    std::memcpy(pe_data.data() + 0x600, &desc, sizeof(desc));

    ImgDelayDescr null_desc{};
    std::memcpy(pe_data.data() + 0x600 + sizeof(ImgDelayDescr), &null_desc, sizeof(null_desc));

    const char* dll = "SHELL32.dll";
    std::memcpy(pe_data.data() + 0x640, dll, std::strlen(dll) + 1);

    // INT: ordinal import (bit 31 set, ordinal = 42)
    u32 ord_thunk = 0x80000000 | 42;
    u32 null_t = 0;
    std::memcpy(pe_data.data() + 0x680, &ord_thunk, 4);
    std::memcpy(pe_data.data() + 0x684, &null_t, 4);

    auto pe_result = parse_pe(pe_data);
    ASSERT_TRUE(pe_result.is_ok());

    DelayImportProcessor proc(memory, thunks);
    auto result = proc.parse_delay_imports(pe_data, pe_result.value(), BASE_ADDRESS);
    ASSERT_TRUE(result.is_ok());

    auto& modules = result.value();
    ASSERT_EQ(modules.size(), 1u);
    ASSERT_EQ(modules[0].entries.size(), 1u);
    EXPECT_TRUE(modules[0].entries[0].is_ordinal);
    EXPECT_EQ(modules[0].entries[0].ordinal, 42u);
}

// 5. Unknown DLL fills trap thunks
TEST_F(DelayImportTest, UnknownDllFillsTrapThunks) {
    auto pe_data = create_base_pe(true);
    fill_single_named_import(pe_data, "UNKNOWN.dll", "SomeFunc");

    auto pe_result = parse_pe(pe_data);
    ASSERT_TRUE(pe_result.is_ok());

    // Do NOT register UNKNOWN.dll in thunk registry
    DelayImportProcessor proc(memory, thunks);
    auto result = proc.resolve_delay_imports(pe_data, pe_result.value(), BASE_ADDRESS);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), 1u);

    // IAT should contain TRAP_THUNK_ADDR
    auto val = memory.read_u32(BASE_ADDRESS + 0x3000);
    ASSERT_TRUE(val.is_ok());
    EXPECT_EQ(val.value(), 0xDEAD0000u);
}

// 6. Unknown function in known DLL fills trap thunk
TEST_F(DelayImportTest, UnknownFunctionFillsTrapThunk) {
    register_dll("KERNEL32.dll", {"GetLastError", "SetLastError"});

    auto pe_data = create_base_pe(true);
    fill_single_named_import(pe_data, "KERNEL32.dll", "NonExistentFunc");

    auto pe_result = parse_pe(pe_data);
    ASSERT_TRUE(pe_result.is_ok());

    DelayImportProcessor proc(memory, thunks);
    auto result = proc.resolve_delay_imports(pe_data, pe_result.value(), BASE_ADDRESS);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), 1u);

    // Should get trap thunk for unknown function
    auto val = memory.read_u32(BASE_ADDRESS + 0x3000);
    ASSERT_TRUE(val.is_ok());
    EXPECT_EQ(val.value(), 0xDEAD0000u);
}

// 7. Multiple delay-loaded DLLs
TEST_F(DelayImportTest, MultipleDelayLoadedDlls) {
    auto pe_data = create_base_pe(true, sizeof(ImgDelayDescr) * 3);

    // Descriptor 1 at file 0x600
    ImgDelayDescr desc1{};
    desc1.grAttrs   = 1;
    desc1.rvaDLLName = 0x2040;
    desc1.rvaINT     = 0x2080;
    desc1.rvaIAT     = 0x3000;
    desc1.rvaHmod    = 0x3080;
    std::memcpy(pe_data.data() + 0x600, &desc1, sizeof(desc1));

    // Descriptor 2 at file 0x620
    ImgDelayDescr desc2{};
    desc2.grAttrs   = 1;
    desc2.rvaDLLName = 0x2140;
    desc2.rvaINT     = 0x2180;
    desc2.rvaIAT     = 0x3010;
    desc2.rvaHmod    = 0x3090;
    std::memcpy(pe_data.data() + 0x620, &desc2, sizeof(desc2));

    // Null terminator at file 0x640
    ImgDelayDescr null_desc{};
    std::memcpy(pe_data.data() + 0x640, &null_desc, sizeof(null_desc));

    // DLL1 name at 0x640 -> RVA 0x2040 -> file 0x640
    // Wait — 0x640 is already the null descriptor now!
    // Let me recalculate: .didata section: RVA 0x2000 = file 0x600
    // desc1 at file 0x600, desc2 at 0x620, null at 0x640 (RVA 0x2040)
    // DLL1 name needs to go somewhere else — RVA 0x2060 = file 0x660
    desc1.rvaDLLName = 0x2060;
    std::memcpy(pe_data.data() + 0x600, &desc1, sizeof(desc1));

    desc2.rvaDLLName = 0x2070;
    std::memcpy(pe_data.data() + 0x620, &desc2, sizeof(desc2));

    // DLL1 name at file 0x660 (RVA 0x2060)
    const char* dll1 = "WS2_32.dll";
    std::memcpy(pe_data.data() + 0x660, dll1, std::strlen(dll1) + 1);

    // DLL2 name at file 0x670 (RVA 0x2070)
    const char* dll2 = "WINMM.dll";
    std::memcpy(pe_data.data() + 0x670, dll2, std::strlen(dll2) + 1);

    // INT1 at file 0x680 (RVA 0x2080): one named import + null
    u32 hn_rva1 = 0x20C0;
    u32 null_t  = 0;
    std::memcpy(pe_data.data() + 0x680, &hn_rva1, 4);
    std::memcpy(pe_data.data() + 0x684, &null_t, 4);

    // INT2 at file 0x780 (RVA 0x2180): one named import + null
    u32 hn_rva2 = 0x20E0;
    std::memcpy(pe_data.data() + 0x780, &hn_rva2, 4);
    std::memcpy(pe_data.data() + 0x784, &null_t, 4);

    // Hint/Name 1 at file 0x6C0 (RVA 0x20C0)
    u16 hint = 0;
    std::memcpy(pe_data.data() + 0x6C0, &hint, 2);
    std::memcpy(pe_data.data() + 0x6C2, "WSAStartup", 11);

    // Hint/Name 2 at file 0x6E0 (RVA 0x20E0)
    std::memcpy(pe_data.data() + 0x6E0, &hint, 2);
    std::memcpy(pe_data.data() + 0x6E2, "timeGetTime", 12);

    auto pe_result = parse_pe(pe_data);
    ASSERT_TRUE(pe_result.is_ok());

    DelayImportProcessor proc(memory, thunks);
    auto result = proc.parse_delay_imports(pe_data, pe_result.value(), BASE_ADDRESS);
    ASSERT_TRUE(result.is_ok());

    auto& modules = result.value();
    ASSERT_EQ(modules.size(), 2u);
    EXPECT_EQ(modules[0].dll_name, "WS2_32.dll");
    EXPECT_EQ(modules[0].entries[0].function_name, "WSAStartup");
    EXPECT_EQ(modules[1].dll_name, "WINMM.dll");
    EXPECT_EQ(modules[1].entries[0].function_name, "timeGetTime");
}

// 8. Module handle written to rvaHmod
TEST_F(DelayImportTest, ModuleHandleWrittenToHmod) {
    register_dll("ADVAPI32.dll", {"RegOpenKeyExW"});

    auto pe_data = create_base_pe(true);
    fill_single_named_import(pe_data, "ADVAPI32.dll", "RegOpenKeyExW");

    auto pe_result = parse_pe(pe_data);
    ASSERT_TRUE(pe_result.is_ok());

    DelayImportProcessor proc(memory, thunks);
    auto result = proc.resolve_delay_imports(pe_data, pe_result.value(), BASE_ADDRESS);
    ASSERT_TRUE(result.is_ok());

    // hmod at RVA 0x3080 should be non-zero (synthetic handle)
    auto val = memory.read_u32(BASE_ADDRESS + 0x3080);
    ASSERT_TRUE(val.is_ok());
    EXPECT_NE(val.value(), 0u);
}

// 9. IAT patched with correct thunk addresses
TEST_F(DelayImportTest, IatPatchedWithCorrectAddresses) {
    register_dll("KERNEL32.dll", {"GetLastError"});

    auto pe_data = create_base_pe(true);
    fill_single_named_import(pe_data, "KERNEL32.dll", "GetLastError");

    auto pe_result = parse_pe(pe_data);
    ASSERT_TRUE(pe_result.is_ok());

    // Get the expected thunk address
    auto expected_addr = thunks.get_thunk_address("KERNEL32.dll", "GetLastError");
    ASSERT_TRUE(expected_addr.is_ok());

    DelayImportProcessor proc(memory, thunks);
    auto result = proc.resolve_delay_imports(pe_data, pe_result.value(), BASE_ADDRESS);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), 1u);

    // IAT at RVA 0x3000 should contain the thunk address
    auto val = memory.read_u32(BASE_ADDRESS + 0x3000);
    ASSERT_TRUE(val.is_ok());
    EXPECT_EQ(val.value(), static_cast<u32>(expected_addr.value()));
}

// 10. All-zero terminator stops processing
TEST_F(DelayImportTest, AllZeroTerminatorStopsProcessing) {
    auto pe_data = create_base_pe(true);

    // Write an all-zero descriptor at the very start
    ImgDelayDescr null_desc{};
    std::memcpy(pe_data.data() + 0x600, &null_desc, sizeof(null_desc));

    auto pe_result = parse_pe(pe_data);
    ASSERT_TRUE(pe_result.is_ok());

    DelayImportProcessor proc(memory, thunks);
    auto result = proc.parse_delay_imports(pe_data, pe_result.value(), BASE_ADDRESS);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().size(), 0u);
}

// 11. VA-based descriptor (grAttrs=0) handled
TEST_F(DelayImportTest, VaBasedDescriptorHandled) {
    register_dll("OLEAUT32.dll", {"SysAllocString"});

    auto pe_data = create_base_pe(true);
    fill_single_named_import(pe_data, "OLEAUT32.dll", "SysAllocString", /*grAttrs=*/0);

    auto pe_result = parse_pe(pe_data);
    ASSERT_TRUE(pe_result.is_ok());

    DelayImportProcessor proc(memory, thunks);

    // Parse should succeed
    auto parse_result = proc.parse_delay_imports(pe_data, pe_result.value(), BASE_ADDRESS);
    ASSERT_TRUE(parse_result.is_ok());
    ASSERT_EQ(parse_result.value().size(), 1u);
    EXPECT_EQ(parse_result.value()[0].dll_name, "OLEAUT32.dll");
    EXPECT_EQ(parse_result.value()[0].entries[0].function_name, "SysAllocString");

    // Resolve should succeed and patch IAT
    auto resolve_result = proc.resolve_delay_imports(pe_data, pe_result.value(), BASE_ADDRESS);
    ASSERT_TRUE(resolve_result.is_ok());
    EXPECT_EQ(resolve_result.value(), 1u);

    auto expected_addr = thunks.get_thunk_address("OLEAUT32.dll", "SysAllocString");
    ASSERT_TRUE(expected_addr.is_ok());

    auto val = memory.read_u32(BASE_ADDRESS + 0x3000);
    ASSERT_TRUE(val.is_ok());
    EXPECT_EQ(val.value(), static_cast<u32>(expected_addr.value()));
}

// 12. Bad RVA in directory returns error
TEST_F(DelayImportTest, BadRvaReturnsError) {
    auto pe_data = create_base_pe(false);

    // Manually set delay import directory to a bogus RVA that doesn't map to any section
    auto pe_result = parse_pe(pe_data);
    ASSERT_TRUE(pe_result.is_ok());

    auto& pe = pe_result.value();
    pe.data_directories[DIR_DELAY_IMPORT].VirtualAddress = 0xFFFF0000;
    pe.data_directories[DIR_DELAY_IMPORT].Size = 64;

    DelayImportProcessor proc(memory, thunks);
    auto result = proc.parse_delay_imports(pe_data, pe, BASE_ADDRESS);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), DelayImportError::InvalidDirectory);
}

// 13. Unknown DLL hmod is zero
TEST_F(DelayImportTest, UnknownDllHmodIsZero) {
    auto pe_data = create_base_pe(true);
    fill_single_named_import(pe_data, "NONEXISTENT.dll", "Foo");

    auto pe_result = parse_pe(pe_data);
    ASSERT_TRUE(pe_result.is_ok());

    DelayImportProcessor proc(memory, thunks);
    auto result = proc.resolve_delay_imports(pe_data, pe_result.value(), BASE_ADDRESS);
    ASSERT_TRUE(result.is_ok());

    // hmod should be 0 for unknown DLL
    auto val = memory.read_u32(BASE_ADDRESS + 0x3080);
    ASSERT_TRUE(val.is_ok());
    EXPECT_EQ(val.value(), 0u);
}

// 14. Resolve returns correct total count
TEST_F(DelayImportTest, ResolveReturnsTotalCount) {
    register_dll("COMCTL32.dll", {"InitCommonControlsEx", "ImageList_Create"});

    auto pe_data = create_base_pe(true);

    // Descriptor
    ImgDelayDescr desc{};
    desc.grAttrs   = 1;
    desc.rvaDLLName = 0x2040;
    desc.rvaINT     = 0x2080;
    desc.rvaIAT     = 0x3000;
    desc.rvaHmod    = 0x3080;
    std::memcpy(pe_data.data() + 0x600, &desc, sizeof(desc));

    ImgDelayDescr null_desc{};
    std::memcpy(pe_data.data() + 0x600 + sizeof(ImgDelayDescr), &null_desc, sizeof(null_desc));

    const char* dll = "COMCTL32.dll";
    std::memcpy(pe_data.data() + 0x640, dll, std::strlen(dll) + 1);

    // INT: 2 named imports + null
    u32 hn_rva1 = 0x20C0;
    u32 hn_rva2 = 0x20E0;
    u32 null_t  = 0;
    std::memcpy(pe_data.data() + 0x680, &hn_rva1, 4);
    std::memcpy(pe_data.data() + 0x684, &hn_rva2, 4);
    std::memcpy(pe_data.data() + 0x688, &null_t, 4);

    u16 hint = 0;
    std::memcpy(pe_data.data() + 0x6C0, &hint, 2);
    std::memcpy(pe_data.data() + 0x6C2, "InitCommonControlsEx", 21);

    std::memcpy(pe_data.data() + 0x6E0, &hint, 2);
    std::memcpy(pe_data.data() + 0x6E2, "ImageList_Create", 17);

    auto pe_result = parse_pe(pe_data);
    ASSERT_TRUE(pe_result.is_ok());

    DelayImportProcessor proc(memory, thunks);
    auto result = proc.resolve_delay_imports(pe_data, pe_result.value(), BASE_ADDRESS);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), 2u);
}

// 15. Error message utility
TEST_F(DelayImportTest, ErrorMessageStrings) {
    EXPECT_STREQ(delay_import_error_message(DelayImportError::Success), "Success");
    EXPECT_STREQ(delay_import_error_message(DelayImportError::NoDelayImports), "No delay imports present");
    EXPECT_STREQ(delay_import_error_message(DelayImportError::InvalidDirectory), "Invalid delay import directory");
    EXPECT_STREQ(delay_import_error_message(DelayImportError::MemoryWriteFailed), "Failed to write memory");
}
