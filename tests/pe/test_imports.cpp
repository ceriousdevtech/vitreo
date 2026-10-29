// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Import resolver and IAT patcher unit tests

#include <gtest/gtest.h>
#include <core/pe/imports/api_set_resolver.hpp>
#include <core/pe/imports/import_resolver.hpp>
#include <core/pe/imports/iat_patcher.hpp>
#include <core/pe/mapper/section_mapper.hpp>
#include <core/pe/parser/pe_parser.hpp>
#include <core/memory/guest_memory.hpp>
#include <cstring>

using namespace vitreo;
using namespace vitreo::pe;

TEST(ApiSetResolverTest, RoutesObservedContractsWithOptionalSuffixAndCasing) {
    const struct { std::string_view contract; std::string_view host; } routes[] = {
#include <core/pe/imports/api_set_observed_hosts.inc>
    };
    for (const auto& route : routes) {
        SCOPED_TRACE(route.contract);
        EXPECT_EQ(resolve_api_set(route.contract), route.host);
        EXPECT_EQ(resolve_api_set(route.contract.substr(0, route.contract.size() - 4)), route.host);
        std::string upper(route.contract);
        for (char& ch : upper) if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
        EXPECT_EQ(resolve_api_set(upper), route.host);
    }
}

TEST(ApiSetResolverTest, RoutesShcoreScalingContractToShcore) {
    EXPECT_EQ(resolve_api_set("api-ms-win-shcore-scaling-l1-1-1.dll"),
              "shcore.dll");
}

TEST(ApiSetResolverTest, RoutesDownlevelShell32ContractToShell32) {
    EXPECT_EQ(resolve_api_set("api-ms-win-downlevel-shell32-l1-1-0.dll"),
              "shell32.dll");
}

TEST(ApiSetResolverTest, RoutesSecuritySystemFunctionsContractToAdvapi32) {
    EXPECT_EQ(resolve_api_set(
                  "api-ms-win-security-systemfunctions-l1-1-0.dll"),
              "advapi32.dll");
}

TEST(ApiSetResolverTest, RoutesSecurityBaseContractToAdvapi32) {
    EXPECT_EQ(resolve_api_set("api-ms-win-security-base-l1-2-2.dll"),
              "advapi32.dll");
}

TEST(ApiSetResolverTest, RoutesPowerBaseContractToPowrprof) {
    EXPECT_EQ(resolve_api_set("api-ms-win-power-base-l1-1-0.dll"),
              "powrprof.dll");
}

TEST(ApiSetResolverTest, RoutesPowerSettingContractToPowrprof) {
    EXPECT_EQ(resolve_api_set("api-ms-win-power-setting-l1-1-0.dll"),
              "powrprof.dll");
    EXPECT_EQ(resolve_api_set("API-MS-WIN-POWER-SETTING-L1-1-1.dll"),
              "powrprof.dll");
}

TEST(ApiSetResolverTest, RoutesMmTimeContractToWinmm) {
    EXPECT_EQ(resolve_api_set("api-ms-win-mm-time-l1-1-0.dll"),
              "winmm.dll");
}

TEST(ApiSetResolverTest, RoutesNtuserSysparamsContractToUser32) {
    EXPECT_EQ(resolve_api_set("api-ms-win-ntuser-sysparams-l1-1-0.dll"),
              "user32.dll");
    EXPECT_EQ(resolve_api_set("ext-ms-win-ntuser-sysparams-ext-l1-1-1.dll"),
              "user32.dll");
}

TEST(ApiSetResolverTest, PhysicalDllsKeepTheirOwnIdentity) {
    // Microsoft Learn "Windows API sets": api-/ext- prefixes distinguish
    // API-set contracts from physical DLL names. Native CRT identity probes
    // also verify independent images/function/data exports on x86 and x64.
    for (const std::string_view name : {
             "msvcr70.dll", "msvcr71.dll", "msvcr80.dll", "MSVCR80.dll",
             "msvcr90.dll", "msvcr100.dll", "msvcr110.dll", "msvcr120.dll",
             "msvcr80d.dll", "msvcr120d.dll", "msvcrt.dll", "msvcp80.dll",
             "ucrtbase_helper.dll", "C:\\private\\ucrtbase.dll",
             "private-api-ms-win-crt-runtime-l1-1-0.dll",
             "private-ext-ms-win-ntuser-window-l1-1-0.dll"}) {
        EXPECT_EQ(resolve_api_set(name), name);
    }
}

TEST(ApiSetResolverTest, PreservesUcrtAbiAndLeavesRedistributableVcruntime) {
    // vcruntime140*.dll ships in the VC++ redistributable, not Windows: it is
    // a real guest module on the DLL search path, never an alias.
    EXPECT_EQ(resolve_api_set("VCRUNTIME140.dll"), "VCRUNTIME140.dll");
    EXPECT_EQ(resolve_api_set("vcruntime140.dll"), "vcruntime140.dll");
    EXPECT_EQ(resolve_api_set("vcruntime140_1.dll"), "vcruntime140_1.dll");
    EXPECT_EQ(resolve_api_set("UCRTBASE.dll"), "ucrtbase.dll");
    EXPECT_EQ(resolve_api_set("API-MS-Win-CRT-Runtime-l1-1-0.dll"),
              "ucrtbase.dll");
    EXPECT_EQ(resolve_api_set("api-ms-win-crt-stdio-l1-1-0.dll"),
              "ucrtbase.dll");
}

TEST(ApiSetResolverTest, RoutesCoreRealtimeContractToKernel32) {
    EXPECT_EQ(resolve_api_set(
                  "api-ms-win-core-realtime-l1-1-1.dll"),
              "kernel32.dll");
}

// ============================================================================
// Test Fixture
// ============================================================================

class ImportResolverTest : public ::testing::Test {
protected:
    GuestMemory memory;
    
    // Build a minimal PE32 with imports
    std::vector<u8> create_test_pe_with_imports() {
        std::vector<u8> data;
        
        // DOS Header (64 bytes)
        DosHeader dos{};
        dos.e_magic = DOS_MAGIC;
        dos.e_lfanew = sizeof(DosHeader);
        
        data.resize(sizeof(DosHeader));
        std::memcpy(data.data(), &dos, sizeof(DosHeader));
        
        // PE Signature (4 bytes)
        u32 pe_sig = PE_SIGNATURE;
        size_t pe_offset = data.size();
        data.resize(data.size() + 4);
        std::memcpy(data.data() + pe_offset, &pe_sig, 4);
        
        // COFF Header (20 bytes)
        CoffHeader coff{};
        coff.Machine = static_cast<u16>(MachineType::I386);
        coff.NumberOfSections = 3;
        coff.SizeOfOptionalHeader = sizeof(OptionalHeader32);
        coff.Characteristics = CoffCharacteristics::EXECUTABLE_IMAGE;
        
        size_t coff_offset = data.size();
        data.resize(data.size() + sizeof(CoffHeader));
        std::memcpy(data.data() + coff_offset, &coff, sizeof(CoffHeader));
        
        // Optional Header (PE32)
        OptionalHeader32 opt{};
        opt.Magic = PE32_MAGIC;
        opt.AddressOfEntryPoint = 0x1000;
        opt.ImageBase = 0x00400000;
        opt.SectionAlignment = 0x1000;
        opt.FileAlignment = 0x200;
        opt.MajorOperatingSystemVersion = 6;
        opt.MinorOperatingSystemVersion = 0;
        opt.SizeOfImage = 0x4000;
        opt.SizeOfHeaders = 0x400;
        opt.Subsystem = static_cast<u16>(Subsystem::WindowsCui);
        opt.NumberOfRvaAndSizes = 16;
        
        // Import directory at RVA 0x2000
        opt.DataDirectory[DIR_IMPORT].VirtualAddress = 0x2000;
        opt.DataDirectory[DIR_IMPORT].Size = sizeof(ImportDescriptor) * 2;  // 1 descriptor + null
        
        size_t opt_offset = data.size();
        data.resize(data.size() + sizeof(OptionalHeader32));
        std::memcpy(data.data() + opt_offset, &opt, sizeof(OptionalHeader32));
        
        // Section 1: .text
        SectionHeader sec1{};
        std::memcpy(sec1.Name, ".text\0\0\0", 8);
        sec1.VirtualSize = 0x100;
        sec1.VirtualAddress = 0x1000;
        sec1.SizeOfRawData = 0x200;
        sec1.PointerToRawData = 0x400;
        sec1.Characteristics = SCN_CNT_CODE | SCN_MEM_EXECUTE | SCN_MEM_READ;
        
        size_t sec1_offset = data.size();
        data.resize(data.size() + sizeof(SectionHeader));
        std::memcpy(data.data() + sec1_offset, &sec1, sizeof(SectionHeader));
        
        // Section 2: .idata (imports)
        SectionHeader sec2{};
        std::memcpy(sec2.Name, ".idata\0\0", 8);
        sec2.VirtualSize = 0x200;
        sec2.VirtualAddress = 0x2000;
        sec2.SizeOfRawData = 0x200;
        sec2.PointerToRawData = 0x600;
        sec2.Characteristics = SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ | SCN_MEM_WRITE;
        
        size_t sec2_offset = data.size();
        data.resize(data.size() + sizeof(SectionHeader));
        std::memcpy(data.data() + sec2_offset, &sec2, sizeof(SectionHeader));
        
        // Section 3: .data (IAT)
        SectionHeader sec3{};
        std::memcpy(sec3.Name, ".data\0\0\0", 8);
        sec3.VirtualSize = 0x100;
        sec3.VirtualAddress = 0x3000;
        sec3.SizeOfRawData = 0x200;
        sec3.PointerToRawData = 0x800;
        sec3.Characteristics = SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ | SCN_MEM_WRITE;
        
        size_t sec3_offset = data.size();
        data.resize(data.size() + sizeof(SectionHeader));
        std::memcpy(data.data() + sec3_offset, &sec3, sizeof(SectionHeader));
        
        // Pad to headers end (0x400)
        data.resize(0x400, 0);
        
        // Section 1 data (.text) - code
        data.resize(0x600, 0);
        
        // Section 2 data (.idata) - import directory
        data.resize(0x800, 0);
        
        // Layout of .idata (RVA 0x2000, file offset 0x600):
        // 0x2000: ImportDescriptor for KERNEL32.dll
        // 0x2014: Null ImportDescriptor (terminator)
        // 0x2028: DLL name "KERNEL32.dll"
        // 0x2040: INT (Import Name Table) - thunks
        // 0x2060: Hint/Name entries
        
        // Import descriptor for KERNEL32.dll
        ImportDescriptor desc{};
        desc.OriginalFirstThunk = 0x2040;  // RVA to INT
        desc.TimeDateStamp = 0;
        desc.ForwarderChain = 0;
        desc.Name = 0x2028;                 // RVA to DLL name
        desc.FirstThunk = 0x3000;           // RVA to IAT (in .data section)
        std::memcpy(data.data() + 0x600, &desc, sizeof(desc));
        
        // Null terminator descriptor
        ImportDescriptor null_desc{};
        std::memcpy(data.data() + 0x600 + sizeof(ImportDescriptor), &null_desc, sizeof(null_desc));
        
        // DLL name at offset 0x628 (RVA 0x2028)
        const char* dll_name = "KERNEL32.dll";
        std::memcpy(data.data() + 0x628, dll_name, strlen(dll_name) + 1);
        
        // INT at offset 0x640 (RVA 0x2040) - 3 imports + null terminator
        // Import 1: GetProcAddress (by name)
        u32 thunk1 = 0x2060;  // RVA to Hint/Name
        std::memcpy(data.data() + 0x640, &thunk1, sizeof(thunk1));
        
        // Import 2: LoadLibraryA (by name)
        u32 thunk2 = 0x2080;  // RVA to Hint/Name
        std::memcpy(data.data() + 0x644, &thunk2, sizeof(thunk2));
        
        // Import 3: ExitProcess (by name)
        u32 thunk3 = 0x20A0;  // RVA to Hint/Name
        std::memcpy(data.data() + 0x648, &thunk3, sizeof(thunk3));
        
        // Null terminator
        u32 null_thunk = 0;
        std::memcpy(data.data() + 0x64C, &null_thunk, sizeof(null_thunk));
        
        // Hint/Name entries
        // Entry 1: GetProcAddress at offset 0x660 (RVA 0x2060)
        u16 hint1 = 0;
        std::memcpy(data.data() + 0x660, &hint1, sizeof(hint1));
        std::memcpy(data.data() + 0x662, "GetProcAddress", 15);
        
        // Entry 2: LoadLibraryA at offset 0x680 (RVA 0x2080)
        u16 hint2 = 0;
        std::memcpy(data.data() + 0x680, &hint2, sizeof(hint2));
        std::memcpy(data.data() + 0x682, "LoadLibraryA", 13);
        
        // Entry 3: ExitProcess at offset 0x6A0 (RVA 0x20A0)
        u16 hint3 = 0;
        std::memcpy(data.data() + 0x6A0, &hint3, sizeof(hint3));
        std::memcpy(data.data() + 0x6A2, "ExitProcess", 12);
        
        // Section 3 data (.data) - IAT
        data.resize(0xA00, 0);
        
        // IAT will be populated by loader/patcher
        // For now, copy the same values as INT
        std::memcpy(data.data() + 0x800, data.data() + 0x640, 16);
        
        return data;
    }
    
    // Build a PE32 with ordinal imports
    std::vector<u8> create_test_pe_with_ordinal_imports() {
        std::vector<u8> data;
        
        // DOS Header
        DosHeader dos{};
        dos.e_magic = DOS_MAGIC;
        dos.e_lfanew = sizeof(DosHeader);
        
        data.resize(sizeof(DosHeader));
        std::memcpy(data.data(), &dos, sizeof(DosHeader));
        
        // PE Signature
        u32 pe_sig = PE_SIGNATURE;
        size_t pe_offset = data.size();
        data.resize(data.size() + 4);
        std::memcpy(data.data() + pe_offset, &pe_sig, 4);
        
        // COFF Header
        CoffHeader coff{};
        coff.Machine = static_cast<u16>(MachineType::I386);
        coff.NumberOfSections = 2;
        coff.SizeOfOptionalHeader = sizeof(OptionalHeader32);
        coff.Characteristics = CoffCharacteristics::EXECUTABLE_IMAGE;
        
        size_t coff_offset = data.size();
        data.resize(data.size() + sizeof(CoffHeader));
        std::memcpy(data.data() + coff_offset, &coff, sizeof(CoffHeader));
        
        // Optional Header
        OptionalHeader32 opt{};
        opt.Magic = PE32_MAGIC;
        opt.AddressOfEntryPoint = 0x1000;
        opt.ImageBase = 0x00400000;
        opt.SectionAlignment = 0x1000;
        opt.FileAlignment = 0x200;
        opt.SizeOfImage = 0x3000;
        opt.SizeOfHeaders = 0x400;
        opt.Subsystem = static_cast<u16>(Subsystem::WindowsCui);
        opt.NumberOfRvaAndSizes = 16;
        
        // Import directory at RVA 0x2000
        opt.DataDirectory[DIR_IMPORT].VirtualAddress = 0x2000;
        opt.DataDirectory[DIR_IMPORT].Size = sizeof(ImportDescriptor) * 2;
        
        size_t opt_offset = data.size();
        data.resize(data.size() + sizeof(OptionalHeader32));
        std::memcpy(data.data() + opt_offset, &opt, sizeof(OptionalHeader32));
        
        // Section 1: .text
        SectionHeader sec1{};
        std::memcpy(sec1.Name, ".text\0\0\0", 8);
        sec1.VirtualSize = 0x100;
        sec1.VirtualAddress = 0x1000;
        sec1.SizeOfRawData = 0x200;
        sec1.PointerToRawData = 0x400;
        sec1.Characteristics = SCN_CNT_CODE | SCN_MEM_EXECUTE | SCN_MEM_READ;
        
        size_t sec1_offset = data.size();
        data.resize(data.size() + sizeof(SectionHeader));
        std::memcpy(data.data() + sec1_offset, &sec1, sizeof(SectionHeader));
        
        // Section 2: .idata
        SectionHeader sec2{};
        std::memcpy(sec2.Name, ".idata\0\0", 8);
        sec2.VirtualSize = 0x200;
        sec2.VirtualAddress = 0x2000;
        sec2.SizeOfRawData = 0x200;
        sec2.PointerToRawData = 0x600;
        sec2.Characteristics = SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ | SCN_MEM_WRITE;
        
        size_t sec2_offset = data.size();
        data.resize(data.size() + sizeof(SectionHeader));
        std::memcpy(data.data() + sec2_offset, &sec2, sizeof(SectionHeader));
        
        // Pad to headers end
        data.resize(0x400, 0);
        
        // .text section
        data.resize(0x600, 0);
        
        // .idata section
        data.resize(0x800, 0);
        
        // Import descriptor
        ImportDescriptor desc{};
        desc.OriginalFirstThunk = 0x2040;  // INT
        desc.Name = 0x2028;                 // DLL name
        desc.FirstThunk = 0x2040;           // IAT (same as INT for simplicity)
        std::memcpy(data.data() + 0x600, &desc, sizeof(desc));
        
        // Null terminator
        ImportDescriptor null_desc{};
        std::memcpy(data.data() + 0x600 + sizeof(ImportDescriptor), &null_desc, sizeof(null_desc));
        
        // DLL name
        std::memcpy(data.data() + 0x628, "USER32.dll", 11);
        
        // INT with ordinal imports (high bit set = ordinal)
        // Ordinal 100
        u32 ord_thunk1 = 0x80000064;  // ORDINAL_FLAG | 100
        std::memcpy(data.data() + 0x640, &ord_thunk1, sizeof(ord_thunk1));
        
        // Ordinal 200
        u32 ord_thunk2 = 0x800000C8;  // ORDINAL_FLAG | 200
        std::memcpy(data.data() + 0x644, &ord_thunk2, sizeof(ord_thunk2));
        
        // Null terminator
        u32 null_thunk = 0;
        std::memcpy(data.data() + 0x648, &null_thunk, sizeof(null_thunk));
        
        return data;
    }
};

// ============================================================================
// Import Parsing Tests
// ============================================================================

TEST_F(ImportResolverTest, ParseImports_ValidPe) {
    auto pe_data = create_test_pe_with_imports();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok()) << parse_error_message(parse_result.error());
    
    const auto& pe = parse_result.value();
    EXPECT_TRUE(pe.has_imports());
    
    auto import_result = parse_imports(pe, pe_data, pe.image_base);
    ASSERT_TRUE(import_result.is_ok()) << import_error_message(import_result.error());
    
    const auto& imports = import_result.value();
    EXPECT_EQ(imports.modules.size(), 1u);
    EXPECT_FALSE(imports.is_64bit);
}

TEST_F(ImportResolverTest, ParseImports_DllName) {
    auto pe_data = create_test_pe_with_imports();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    auto import_result = parse_imports(parse_result.value(), pe_data, 0x00400000);
    ASSERT_TRUE(import_result.is_ok());
    
    EXPECT_EQ(import_result.value().modules[0].dll_name, "KERNEL32.dll");
}

TEST_F(ImportResolverTest, ParseImports_FunctionNames) {
    auto pe_data = create_test_pe_with_imports();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    auto import_result = parse_imports(parse_result.value(), pe_data, 0x00400000);
    ASSERT_TRUE(import_result.is_ok());
    
    const auto& module = import_result.value().modules[0];
    EXPECT_EQ(module.imports.size(), 3u);
    
    EXPECT_EQ(module.imports[0].function_name, "GetProcAddress");
    EXPECT_FALSE(module.imports[0].is_ordinal);
    
    EXPECT_EQ(module.imports[1].function_name, "LoadLibraryA");
    EXPECT_FALSE(module.imports[1].is_ordinal);
    
    EXPECT_EQ(module.imports[2].function_name, "ExitProcess");
    EXPECT_FALSE(module.imports[2].is_ordinal);
}

TEST_F(ImportResolverTest, ParseImports_LongMsvcMangledName) {
    auto pe_data = create_test_pe_with_imports();
    const std::string long_name = "?" + std::string(282, 'A') + "Z";

    const u32 null_thunk = 0;
    std::memcpy(pe_data.data() + 0x644, &null_thunk, sizeof(null_thunk));
    std::memcpy(pe_data.data() + 0x662, long_name.c_str(),
                long_name.size() + 1);

    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());

    auto import_result =
        parse_imports(parse_result.value(), pe_data, 0x00400000);
    ASSERT_TRUE(import_result.is_ok())
        << import_error_message(import_result.error());
    ASSERT_EQ(import_result.value().modules[0].imports.size(), 1u);
    EXPECT_EQ(import_result.value().modules[0].imports[0].function_name,
              long_name);
}

TEST_F(ImportResolverTest, ParseImports_IatAddresses) {
    auto pe_data = create_test_pe_with_imports();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    u64 base = 0x00400000;
    auto import_result = parse_imports(parse_result.value(), pe_data, base);
    ASSERT_TRUE(import_result.is_ok());
    
    const auto& module = import_result.value().modules[0];
    
    // IAT is at RVA 0x3000, each entry is 4 bytes (32-bit)
    EXPECT_EQ(module.imports[0].iat_address, base + 0x3000);
    EXPECT_EQ(module.imports[1].iat_address, base + 0x3004);
    EXPECT_EQ(module.imports[2].iat_address, base + 0x3008);
}

TEST_F(ImportResolverTest, ParseImports_Ordinals) {
    auto pe_data = create_test_pe_with_ordinal_imports();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    auto import_result = parse_imports(parse_result.value(), pe_data, 0x00400000);
    ASSERT_TRUE(import_result.is_ok());
    
    const auto& module = import_result.value().modules[0];
    EXPECT_EQ(module.dll_name, "USER32.dll");
    EXPECT_EQ(module.imports.size(), 2u);
    
    EXPECT_TRUE(module.imports[0].is_ordinal);
    EXPECT_EQ(module.imports[0].ordinal, 100u);
    EXPECT_TRUE(module.imports[0].function_name.empty());
    
    EXPECT_TRUE(module.imports[1].is_ordinal);
    EXPECT_EQ(module.imports[1].ordinal, 200u);
}

TEST_F(ImportResolverTest, ParseImports_TotalCount) {
    auto pe_data = create_test_pe_with_imports();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    auto import_result = parse_imports(parse_result.value(), pe_data, 0x00400000);
    ASSERT_TRUE(import_result.is_ok());
    
    EXPECT_EQ(import_result.value().total_imports, 3u);
}

// ============================================================================
// IAT Patching Tests
// ============================================================================

TEST_F(ImportResolverTest, PatchIat_AllResolved) {
    auto pe_data = create_test_pe_with_imports();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    const auto& pe = parse_result.value();
    
    // Map PE into memory
    auto map_result = map_pe(memory, pe, pe_data, pe.image_base);
    ASSERT_TRUE(map_result.is_ok());
    
    // Parse imports
    auto import_result = parse_imports(pe, pe_data, pe.image_base);
    ASSERT_TRUE(import_result.is_ok());
    
    // Mock resolver that returns predictable addresses
    ImportCallback resolver = [](std::string_view dll, std::string_view func, u16 ord) -> u64 {
        (void)dll;
        (void)ord;
        if (func == "GetProcAddress") return 0xDEAD0001;
        if (func == "LoadLibraryA") return 0xDEAD0002;
        if (func == "ExitProcess") return 0xDEAD0003;
        return 0;
    };
    
    // Patch IAT
    IatPatcher patcher(memory);
    auto patch_result = patcher.patch(import_result.value(), resolver);
    ASSERT_TRUE(patch_result.is_ok());
    
    const auto& stats = patch_result.value();
    EXPECT_EQ(stats.stats.imports_resolved, 3u);
    EXPECT_EQ(stats.stats.imports_unresolved, 0u);
    EXPECT_TRUE(stats.unresolved.empty());
    
    // Verify IAT was patched
    u64 iat_base = pe.image_base + 0x3000;
    
    auto val1 = memory.read_u32(iat_base);
    ASSERT_TRUE(val1.is_ok());
    EXPECT_EQ(val1.value(), 0xDEAD0001u);
    
    auto val2 = memory.read_u32(iat_base + 4);
    ASSERT_TRUE(val2.is_ok());
    EXPECT_EQ(val2.value(), 0xDEAD0002u);
    
    auto val3 = memory.read_u32(iat_base + 8);
    ASSERT_TRUE(val3.is_ok());
    EXPECT_EQ(val3.value(), 0xDEAD0003u);
}

TEST_F(ImportResolverTest, PatchIat_SomeUnresolved) {
    auto pe_data = create_test_pe_with_imports();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    const auto& pe = parse_result.value();
    
    // Map PE
    auto map_result = map_pe(memory, pe, pe_data, pe.image_base);
    ASSERT_TRUE(map_result.is_ok());
    
    // Parse imports
    auto import_result = parse_imports(pe, pe_data, pe.image_base);
    ASSERT_TRUE(import_result.is_ok());
    
    // Resolver that only resolves some functions
    ImportCallback resolver = [](std::string_view, std::string_view func, u16) -> u64 {
        if (func == "GetProcAddress") return 0xBEEF0001;
        return 0;  // Unresolved
    };
    
    // Patch IAT
    auto patch_result = patch_iat(memory, import_result.value(), resolver);
    ASSERT_TRUE(patch_result.is_ok());
    
    const auto& result = patch_result.value();
    EXPECT_EQ(result.stats.imports_resolved, 1u);
    EXPECT_EQ(result.stats.imports_unresolved, 2u);
    EXPECT_EQ(result.unresolved.size(), 2u);
    
    // Check unresolved list
    EXPECT_EQ(result.unresolved[0].function_name, "LoadLibraryA");
    EXPECT_EQ(result.unresolved[1].function_name, "ExitProcess");
}

TEST_F(ImportResolverTest, ResolveAndPatch_Integration) {
    auto pe_data = create_test_pe_with_imports();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    const auto& pe = parse_result.value();
    
    // Map PE
    auto map_result = map_pe(memory, pe, pe_data, pe.image_base);
    ASSERT_TRUE(map_result.is_ok());
    
    // Resolve and patch in one call
    ImportCallback resolver = [](std::string_view, std::string_view func, u16) -> u64 {
        if (func == "GetProcAddress") return 0xAAAA0001;
        if (func == "LoadLibraryA") return 0xAAAA0002;
        if (func == "ExitProcess") return 0xAAAA0003;
        return 0;
    };
    
    auto result = resolve_and_patch_imports(memory, pe, pe_data, pe.image_base, resolver);
    ASSERT_TRUE(result.is_ok());
    
    EXPECT_EQ(result.value().stats.imports_resolved, 3u);
}

// ============================================================================
// Error Message Tests
// ============================================================================

TEST(ImportErrorTest, ErrorMessages) {
    EXPECT_STREQ(import_error_message(ImportError::Success), "Success");
    EXPECT_STREQ(import_error_message(ImportError::NoImports), "No imports present");
    EXPECT_STREQ(import_error_message(ImportError::InvalidImportDirectory), "Invalid import directory");
    EXPECT_STREQ(import_error_message(ImportError::InvalidThunkData), "Invalid thunk data");
}

// ============================================================================
// HasImports Tests
// ============================================================================

TEST_F(ImportResolverTest, HasImports_True) {
    auto pe_data = create_test_pe_with_imports();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    EXPECT_TRUE(ImportResolver::has_imports(parse_result.value()));
}
