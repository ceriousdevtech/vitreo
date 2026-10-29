// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Section mapper unit tests

#include <gtest/gtest.h>
#include <core/pe/mapper/section_mapper.hpp>
#include <core/pe/parser/pe_parser.hpp>
#include <core/memory/guest_memory.hpp>
#include <cstring>

using namespace vitreo;
using namespace vitreo::pe;

// ============================================================================
// Test Fixture
// ============================================================================

class SectionMapperTest : public ::testing::Test {
protected:
    GuestMemory memory;
    
    // Build a minimal valid PE32 file with multiple sections
    std::vector<u8> create_test_pe() {
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
        
        size_t opt_offset = data.size();
        data.resize(data.size() + sizeof(OptionalHeader32));
        std::memcpy(data.data() + opt_offset, &opt, sizeof(OptionalHeader32));
        
        // Section 1: .text (code)
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
        
        // Section 2: .data (initialized data)
        SectionHeader sec2{};
        std::memcpy(sec2.Name, ".data\0\0\0", 8);
        sec2.VirtualSize = 0x80;
        sec2.VirtualAddress = 0x2000;
        sec2.SizeOfRawData = 0x200;
        sec2.PointerToRawData = 0x600;
        sec2.Characteristics = SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ | SCN_MEM_WRITE;
        
        size_t sec2_offset = data.size();
        data.resize(data.size() + sizeof(SectionHeader));
        std::memcpy(data.data() + sec2_offset, &sec2, sizeof(SectionHeader));
        
        // Section 3: .rdata (read-only data)
        SectionHeader sec3{};
        std::memcpy(sec3.Name, ".rdata\0\0", 8);
        sec3.VirtualSize = 0x40;
        sec3.VirtualAddress = 0x3000;
        sec3.SizeOfRawData = 0x200;
        sec3.PointerToRawData = 0x800;
        sec3.Characteristics = SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ;
        
        size_t sec3_offset = data.size();
        data.resize(data.size() + sizeof(SectionHeader));
        std::memcpy(data.data() + sec3_offset, &sec3, sizeof(SectionHeader));
        
        // Pad to headers end (0x400)
        data.resize(0x400, 0);
        
        // Section 1 data (.text) - some "code"
        data.resize(0x600, 0);
        // Write some test bytes
        data[0x400] = 0x55;  // push ebp
        data[0x401] = 0x8B;  // mov ebp, esp
        data[0x402] = 0xEC;
        data[0x403] = 0xC3;  // ret
        
        // Section 2 data (.data) - some data
        data.resize(0x800, 0);
        data[0x600] = 0xDE;
        data[0x601] = 0xAD;
        data[0x602] = 0xBE;
        data[0x603] = 0xEF;
        
        // Section 3 data (.rdata) - read-only data
        data.resize(0xA00, 0);
        data[0x800] = 'H';
        data[0x801] = 'E';
        data[0x802] = 'L';
        data[0x803] = 'L';
        data[0x804] = 'O';
        
        return data;
    }
};

// ============================================================================
// Basic Mapping Tests
// ============================================================================

TEST_F(SectionMapperTest, MapPe_ValidPe32) {
    auto pe_data = create_test_pe();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok()) << parse_error_message(parse_result.error());
    
    const auto& pe = parse_result.value();
    
    SectionMapper mapper(memory);
    auto map_result = mapper.map(pe, pe_data);
    
    ASSERT_TRUE(map_result.is_ok()) << map_error_message(map_result.error());
    
    const auto& image = map_result.value();
    EXPECT_EQ(image.base_address, 0x00400000u);
    EXPECT_EQ(image.entry_point, 0x00401000u);  // base + 0x1000
    EXPECT_EQ(image.sections.size(), 3u);
}

TEST_F(SectionMapperTest, MapPe_SectionCount) {
    auto pe_data = create_test_pe();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    const auto& pe = parse_result.value();
    
    auto map_result = map_pe(memory, pe, pe_data);
    ASSERT_TRUE(map_result.is_ok());
    
    const auto& image = map_result.value();
    EXPECT_EQ(image.sections.size(), 3u);
    
    // Verify section names
    EXPECT_STREQ(image.sections[0].name, ".text");
    EXPECT_STREQ(image.sections[1].name, ".data");
    EXPECT_STREQ(image.sections[2].name, ".rdata");
}

TEST_F(SectionMapperTest, MapPe_SectionAddresses) {
    auto pe_data = create_test_pe();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    const auto& pe = parse_result.value();
    
    auto map_result = map_pe(memory, pe, pe_data);
    ASSERT_TRUE(map_result.is_ok());
    
    const auto& image = map_result.value();
    
    // Verify section addresses
    EXPECT_EQ(image.sections[0].guest_address, 0x00401000u);  // .text
    EXPECT_EQ(image.sections[1].guest_address, 0x00402000u);  // .data
    EXPECT_EQ(image.sections[2].guest_address, 0x00403000u);  // .rdata
}

TEST_F(SectionMapperTest, MapPe_SectionProtection) {
    auto pe_data = create_test_pe();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    const auto& pe = parse_result.value();
    
    auto map_result = map_pe(memory, pe, pe_data);
    ASSERT_TRUE(map_result.is_ok());
    
    const auto& image = map_result.value();
    
    // .text should be readable and executable
    EXPECT_TRUE((static_cast<u32>(image.sections[0].protection) & 
                 static_cast<u32>(MemoryProtection::Read)) != 0);
    EXPECT_TRUE((static_cast<u32>(image.sections[0].protection) & 
                 static_cast<u32>(MemoryProtection::Execute)) != 0);
    
    // .data should be readable and writable
    EXPECT_TRUE((static_cast<u32>(image.sections[1].protection) & 
                 static_cast<u32>(MemoryProtection::Read)) != 0);
    EXPECT_TRUE((static_cast<u32>(image.sections[1].protection) & 
                 static_cast<u32>(MemoryProtection::Write)) != 0);
    
    // .text must NOT be writable: patching code without VirtualProtect faults
    // on Windows, and the JIT's self-modifying-code detection depends on the
    // section staying write-protected.
    EXPECT_FALSE((static_cast<u32>(image.sections[0].protection) &
                  static_cast<u32>(MemoryProtection::Write)) != 0);

    // .rdata is marked read-only in the PE. VirtualQuery must report
    // PAGE_READONLY (Chromium ProtectedMemory / CEF "prot" section CHECK).
    // In-place COW stores without VirtualProtect are not emulated here.
    EXPECT_TRUE((static_cast<u32>(image.sections[2].protection) &
                 static_cast<u32>(MemoryProtection::Read)) != 0);
    EXPECT_FALSE((static_cast<u32>(image.sections[2].protection) &
                  static_cast<u32>(MemoryProtection::Write)) != 0);
    EXPECT_FALSE((static_cast<u32>(image.sections[2].protection) &
                  static_cast<u32>(MemoryProtection::Execute)) != 0);

    EXPECT_TRUE(memory.has_protection(
        image.base_address + 0x3000, 1, MemoryProtection::Read));
    EXPECT_FALSE(memory.has_protection(
        image.base_address + 0x3000, 1, MemoryProtection::Write));
    auto wr = memory.write_u8(image.base_address + 0x3000, 0x00);
    ASSERT_TRUE(wr.is_err());
    EXPECT_EQ(wr.error(), Error::ProtectionFault);
}

TEST_F(SectionMapperTest, MapPe_HeaderPagesReadOnly) {
    auto pe_data = create_test_pe();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());

    auto map_result = map_pe(memory, parse_result.value(), pe_data);
    ASSERT_TRUE(map_result.is_ok());
    const auto& image = map_result.value();

    // SizeOfHeaders=0x400, first section at 0x1000 → one header page.
    EXPECT_EQ(image.header_size, 0x1000u);
    EXPECT_TRUE(memory.has_protection(
        image.base_address, 1, MemoryProtection::Read));
    EXPECT_FALSE(memory.has_protection(
        image.base_address, 1, MemoryProtection::Write));

    // A store through ImageBase must fault — Petite (and similar packers)
    // install SEH and use that ACCESS_VIOLATION as control flow. Leaving the
    // header writable lets the store corrupt the packed bitstream instead.
    auto wr = memory.write_u8(image.base_address, 0x00);
    ASSERT_TRUE(wr.is_err());
    EXPECT_EQ(wr.error(), Error::ProtectionFault);

    // Headers remain readable (MZ signature).
    auto mz = memory.read_u16(image.base_address);
    ASSERT_TRUE(mz.is_ok());
    EXPECT_EQ(mz.value(), 0x5A4Du);
}

// ============================================================================
// Data Verification Tests
// ============================================================================

TEST_F(SectionMapperTest, MapPe_TextSectionData) {
    auto pe_data = create_test_pe();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    auto map_result = map_pe(memory, parse_result.value(), pe_data);
    ASSERT_TRUE(map_result.is_ok());
    
    // Read code bytes from .text section
    u64 text_addr = 0x00401000;
    
    auto b0 = memory.read_u8(text_addr);
    auto b1 = memory.read_u8(text_addr + 1);
    auto b2 = memory.read_u8(text_addr + 2);
    auto b3 = memory.read_u8(text_addr + 3);
    
    ASSERT_TRUE(b0.is_ok());
    ASSERT_TRUE(b1.is_ok());
    ASSERT_TRUE(b2.is_ok());
    ASSERT_TRUE(b3.is_ok());
    
    EXPECT_EQ(b0.value(), 0x55u);  // push ebp
    EXPECT_EQ(b1.value(), 0x8Bu);  // mov ebp, esp
    EXPECT_EQ(b2.value(), 0xECu);
    EXPECT_EQ(b3.value(), 0xC3u);  // ret
}

TEST_F(SectionMapperTest, MapPe_DataSectionData) {
    auto pe_data = create_test_pe();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    auto map_result = map_pe(memory, parse_result.value(), pe_data);
    ASSERT_TRUE(map_result.is_ok());
    
    // Read data from .data section
    u64 data_addr = 0x00402000;
    
    auto val = memory.read_u32(data_addr);
    ASSERT_TRUE(val.is_ok());
    EXPECT_EQ(val.value(), 0xEFBEADDEu);  // DEADBEEF in little-endian
}

TEST_F(SectionMapperTest, MapPe_RdataSectionData) {
    auto pe_data = create_test_pe();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    auto map_result = map_pe(memory, parse_result.value(), pe_data);
    ASSERT_TRUE(map_result.is_ok());
    
    // Read string from .rdata section
    u64 rdata_addr = 0x00403000;
    
    auto b0 = memory.read_u8(rdata_addr);
    auto b1 = memory.read_u8(rdata_addr + 1);
    auto b2 = memory.read_u8(rdata_addr + 2);
    auto b3 = memory.read_u8(rdata_addr + 3);
    auto b4 = memory.read_u8(rdata_addr + 4);
    
    ASSERT_TRUE(b0.is_ok());
    ASSERT_TRUE(b1.is_ok());
    ASSERT_TRUE(b2.is_ok());
    ASSERT_TRUE(b3.is_ok());
    ASSERT_TRUE(b4.is_ok());
    
    EXPECT_EQ(static_cast<char>(b0.value()), 'H');
    EXPECT_EQ(static_cast<char>(b1.value()), 'E');
    EXPECT_EQ(static_cast<char>(b2.value()), 'L');
    EXPECT_EQ(static_cast<char>(b3.value()), 'L');
    EXPECT_EQ(static_cast<char>(b4.value()), 'O');
}

// ============================================================================
// Custom Load Address Tests
// ============================================================================

TEST_F(SectionMapperTest, MapPe_CustomLoadAddress) {
    auto pe_data = create_test_pe();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    const auto& pe = parse_result.value();
    
    // Load at a different address
    u64 custom_base = 0x10000000;
    auto map_result = map_pe(memory, pe, pe_data, custom_base);
    
    ASSERT_TRUE(map_result.is_ok());
    
    const auto& image = map_result.value();
    EXPECT_EQ(image.base_address, custom_base);
    EXPECT_EQ(image.entry_point, custom_base + 0x1000);
    
    // Verify sections are at correct addresses
    EXPECT_EQ(image.sections[0].guest_address, custom_base + 0x1000);
    EXPECT_EQ(image.sections[1].guest_address, custom_base + 0x2000);
    EXPECT_EQ(image.sections[2].guest_address, custom_base + 0x3000);
}

TEST_F(SectionMapperTest, MapPe_DataAtCustomAddress) {
    auto pe_data = create_test_pe();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    u64 custom_base = 0x20000000;
    auto map_result = map_pe(memory, parse_result.value(), pe_data, custom_base);
    ASSERT_TRUE(map_result.is_ok());
    
    // Verify code is at new address
    u64 text_addr = custom_base + 0x1000;
    auto b0 = memory.read_u8(text_addr);
    ASSERT_TRUE(b0.is_ok());
    EXPECT_EQ(b0.value(), 0x55u);  // push ebp
}

// ============================================================================
// Unmapping Tests
// ============================================================================

TEST_F(SectionMapperTest, UnmapImage) {
    auto pe_data = create_test_pe();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    SectionMapper mapper(memory);
    auto map_result = mapper.map(parse_result.value(), pe_data);
    ASSERT_TRUE(map_result.is_ok());
    
    const auto& image = map_result.value();
    
    // Verify memory is mapped
    EXPECT_TRUE(memory.is_mapped(image.sections[0].guest_address));
    
    // Unmap
    auto unmap_result = mapper.unmap(image);
    EXPECT_TRUE(unmap_result.is_ok());
    
    // Verify memory is unmapped
    EXPECT_FALSE(memory.is_mapped(image.sections[0].guest_address));
}

// ============================================================================
// Entry Point Tests
// ============================================================================

TEST_F(SectionMapperTest, HasEntryPoint) {
    auto pe_data = create_test_pe();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    auto map_result = map_pe(memory, parse_result.value(), pe_data);
    ASSERT_TRUE(map_result.is_ok());
    
    const auto& image = map_result.value();
    EXPECT_TRUE(image.has_entry_point());
}

// ============================================================================
// Contiguous image / packer contracts
// ============================================================================

TEST_F(SectionMapperTest, MapPe_CrossSectionAccess) {
    // Windows maps SizeOfImage as one region; dword stores may straddle
    // section VAs. Separate per-section GuestMemory regions reject that.
    auto pe_data = create_test_pe();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());

    auto map_result = map_pe(memory, parse_result.value(), pe_data);
    ASSERT_TRUE(map_result.is_ok());

    // Whole image must be addressable (headers through last section page).
    EXPECT_TRUE(memory.is_mapped(0x00400000u));
    EXPECT_TRUE(memory.is_mapped(0x00403FFFu));

    // Contiguous image mapping: a dword read may straddle .data / .rdata.
    // Writes into .rdata fault (PAGE_READONLY) unless VirtualProtect'd.
    const u64 edge = 0x00402FFEu;
    auto seed = memory.write_u16(edge, 0x3344u);  // wholly inside .data
    ASSERT_TRUE(seed.is_ok());
    auto low = memory.read_u16(edge);
    auto high = memory.read_u16(edge + 2);  // crosses into .rdata
    ASSERT_TRUE(low.is_ok());
    ASSERT_TRUE(high.is_ok());
    EXPECT_EQ(low.value(), 0x3344u);

    auto straddling_write = memory.write_u32(edge, 0x11223344u);
    ASSERT_TRUE(straddling_write.is_err())
        << "store into PAGE_READONLY .rdata half must fault";
    EXPECT_EQ(straddling_write.error(), Error::ProtectionFault);
}

TEST_F(SectionMapperTest, MapPe_RawSizeExceedsVirtualSize) {
    // Packer stubs (Petite) often set SizeOfRawData > VirtualSize. The mapped
    // image must still receive the full raw payload, not truncate to VS.
    auto pe_data = create_test_pe();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    auto pe = parse_result.value();

    // Enlarge .text raw payload past VirtualSize and stamp a marker at the
    // end of the raw bytes (past VirtualSize).
    ASSERT_FALSE(pe.sections.empty());
    pe.sections[0].virtual_size = 0x100;
    pe.sections[0].raw_size = 0x200;
    constexpr u8 kMarker = 0xA5;
    pe_data[0x400 + 0x1FF] = kMarker;

    auto map_result = map_pe(memory, pe, pe_data);
    ASSERT_TRUE(map_result.is_ok());

    auto byte = memory.read_u8(0x00401000 + 0x1FF);
    ASSERT_TRUE(byte.is_ok());
    EXPECT_EQ(byte.value(), kMarker);
}

// ============================================================================
// Error Message Tests
// ============================================================================

TEST(MapErrorTest, ErrorMessages) {
    EXPECT_STREQ(map_error_message(MapError::Success), "Success");
    EXPECT_STREQ(map_error_message(MapError::InvalidPe), "Invalid PE file");
    EXPECT_STREQ(map_error_message(MapError::MemoryMapFailed), "Failed to map memory region");
}
