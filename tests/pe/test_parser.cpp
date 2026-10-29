// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// PE parser unit tests

#include <gtest/gtest.h>
#include <core/pe/parser/pe_parser.hpp>
#include <vector>
#include <cstring>

using namespace vitreo;
using namespace vitreo::pe;

// ============================================================================
// Test Fixture
// ============================================================================

class PeParserTest : public ::testing::Test {
protected:
    // Build a minimal valid PE32 file in memory
    std::vector<u8> create_minimal_pe32() {
        std::vector<u8> data;
        
        // DOS Header (64 bytes)
        DosHeader dos{};
        dos.e_magic = DOS_MAGIC;  // 'MZ'
        dos.e_lfanew = sizeof(DosHeader);  // PE header right after DOS header
        
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
        coff.NumberOfSections = 1;
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
        opt.MajorSubsystemVersion = 6;
        opt.MinorSubsystemVersion = 0;
        opt.SizeOfImage = 0x3000;
        opt.SizeOfHeaders = 0x200;
        opt.Subsystem = static_cast<u16>(Subsystem::WindowsCui);
        opt.DllCharacteristics = 0;
        opt.SizeOfStackReserve = 0x100000;
        opt.SizeOfStackCommit = 0x1000;
        opt.SizeOfHeapReserve = 0x100000;
        opt.SizeOfHeapCommit = 0x1000;
        opt.NumberOfRvaAndSizes = 16;
        
        size_t opt_offset = data.size();
        data.resize(data.size() + sizeof(OptionalHeader32));
        std::memcpy(data.data() + opt_offset, &opt, sizeof(OptionalHeader32));
        
        // Section Header
        SectionHeader sec{};
        std::memcpy(sec.Name, ".text\0\0\0", 8);
        sec.VirtualSize = 0x100;
        sec.VirtualAddress = 0x1000;
        sec.SizeOfRawData = 0x200;
        sec.PointerToRawData = 0x200;
        sec.Characteristics = SCN_CNT_CODE | SCN_MEM_EXECUTE | SCN_MEM_READ;
        
        size_t sec_offset = data.size();
        data.resize(data.size() + sizeof(SectionHeader));
        std::memcpy(data.data() + sec_offset, &sec, sizeof(SectionHeader));
        
        // Pad to file alignment and add section data
        data.resize(0x200 + 0x200, 0x00);
        
        return data;
    }
    
    // Build a minimal valid PE64 file in memory
    std::vector<u8> create_minimal_pe64() {
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
        coff.Machine = static_cast<u16>(MachineType::AMD64);
        coff.NumberOfSections = 2;
        coff.SizeOfOptionalHeader = sizeof(OptionalHeader64);
        coff.Characteristics = CoffCharacteristics::EXECUTABLE_IMAGE | 
                               CoffCharacteristics::LARGE_ADDRESS_AWARE;
        
        size_t coff_offset = data.size();
        data.resize(data.size() + sizeof(CoffHeader));
        std::memcpy(data.data() + coff_offset, &coff, sizeof(CoffHeader));
        
        // Optional Header (PE64)
        OptionalHeader64 opt{};
        opt.Magic = PE32PLUS_MAGIC;
        opt.AddressOfEntryPoint = 0x1000;
        opt.ImageBase = 0x0000000140000000ULL;
        opt.SectionAlignment = 0x1000;
        opt.FileAlignment = 0x200;
        opt.MajorOperatingSystemVersion = 6;
        opt.MinorOperatingSystemVersion = 0;
        opt.MajorSubsystemVersion = 6;
        opt.MinorSubsystemVersion = 0;
        opt.SizeOfImage = 0x4000;
        opt.SizeOfHeaders = 0x400;
        opt.Subsystem = static_cast<u16>(Subsystem::WindowsGui);
        opt.DllCharacteristics = DYNAMIC_BASE | NX_COMPAT;
        opt.SizeOfStackReserve = 0x100000;
        opt.SizeOfStackCommit = 0x1000;
        opt.SizeOfHeapReserve = 0x100000;
        opt.SizeOfHeapCommit = 0x1000;
        opt.NumberOfRvaAndSizes = 16;
        
        size_t opt_offset = data.size();
        data.resize(data.size() + sizeof(OptionalHeader64));
        std::memcpy(data.data() + opt_offset, &opt, sizeof(OptionalHeader64));
        
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
        
        // Section 2: .data
        SectionHeader sec2{};
        std::memcpy(sec2.Name, ".data\0\0\0", 8);
        sec2.VirtualSize = 0x50;
        sec2.VirtualAddress = 0x2000;
        sec2.SizeOfRawData = 0x200;
        sec2.PointerToRawData = 0x600;
        sec2.Characteristics = SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ | SCN_MEM_WRITE;
        
        size_t sec2_offset = data.size();
        data.resize(data.size() + sizeof(SectionHeader));
        std::memcpy(data.data() + sec2_offset, &sec2, sizeof(SectionHeader));
        
        // Pad to file alignment and add section data
        data.resize(0x600 + 0x200, 0x00);
        
        return data;
    }
};

// ============================================================================
// DOS Header Tests
// ============================================================================

TEST_F(PeParserTest, ParseDosHeader_ValidHeader) {
    auto data = create_minimal_pe32();
    auto result = parse_dos_header(data);
    
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().e_magic, DOS_MAGIC);
    EXPECT_EQ(result.value().e_lfanew, sizeof(DosHeader));
}

TEST_F(PeParserTest, ParseDosHeader_EmptyFile) {
    std::vector<u8> data;
    auto result = parse_dos_header(data);
    
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), ParseError::TruncatedFile);
}

TEST_F(PeParserTest, ParseDosHeader_InvalidMagic) {
    std::vector<u8> data(64, 0);
    auto result = parse_dos_header(data);
    
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), ParseError::InvalidDosHeader);
}

TEST_F(PeParserTest, ParseDosHeader_TruncatedFile) {
    std::vector<u8> data = { 'M', 'Z' };  // Only 2 bytes
    auto result = parse_dos_header(data);
    
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), ParseError::TruncatedFile);
}

// ============================================================================
// PE Signature Tests
// ============================================================================

TEST_F(PeParserTest, ValidatePeSignature_Valid) {
    auto data = create_minimal_pe32();
    DosHeader dos{};
    std::memcpy(&dos, data.data(), sizeof(DosHeader));
    
    auto result = validate_pe_signature(data, dos.e_lfanew);
    ASSERT_TRUE(result.is_ok());
}

TEST_F(PeParserTest, ValidatePeSignature_Invalid) {
    std::vector<u8> data(128, 0);
    data[0] = 'M';
    data[1] = 'Z';
    
    auto result = validate_pe_signature(data, 64);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), ParseError::InvalidPeSignature);
}

TEST_F(PeParserTest, ValidatePeSignature_Truncated) {
    std::vector<u8> data(66, 0);
    data[0] = 'M';
    data[1] = 'Z';
    
    // DOS header says PE is at offset 64, but file is too short
    auto result = validate_pe_signature(data, 64);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), ParseError::TruncatedFile);
}

// ============================================================================
// COFF Header Tests
// ============================================================================

TEST_F(PeParserTest, ParseCoffHeader_ValidI386) {
    auto data = create_minimal_pe32();
    u32 coff_offset = sizeof(DosHeader) + 4;  // After DOS header and PE sig
    
    auto result = parse_coff_header(data, coff_offset);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().Machine, static_cast<u16>(MachineType::I386));
}

TEST_F(PeParserTest, ParseCoffHeader_ValidAMD64) {
    auto data = create_minimal_pe64();
    u32 coff_offset = sizeof(DosHeader) + 4;
    
    auto result = parse_coff_header(data, coff_offset);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().Machine, static_cast<u16>(MachineType::AMD64));
}

TEST_F(PeParserTest, ParseCoffHeader_UnsupportedMachine) {
    auto data = create_minimal_pe32();
    u32 coff_offset = sizeof(DosHeader) + 4;
    
    // Change machine type to ARM (not supported)
    u16 arm_machine = 0x01c0;  // IMAGE_FILE_MACHINE_ARM
    std::memcpy(data.data() + coff_offset, &arm_machine, sizeof(u16));
    
    auto result = parse_coff_header(data, coff_offset);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), ParseError::UnsupportedMachine);
}

// ============================================================================
// Full PE Parse Tests - PE32
// ============================================================================

TEST_F(PeParserTest, ParsePe_ValidPe32) {
    auto data = create_minimal_pe32();
    auto result = parse_pe(data);
    
    ASSERT_TRUE(result.is_ok()) << parse_error_message(result.error());
    
    const auto& pe = result.value();
    EXPECT_FALSE(pe.is_64bit);
    EXPECT_EQ(pe.machine, MachineType::I386);
    EXPECT_EQ(pe.entry_point_rva, 0x1000u);
    EXPECT_EQ(pe.image_base, 0x00400000u);
    EXPECT_EQ(pe.section_alignment, 0x1000u);
    EXPECT_EQ(pe.file_alignment, 0x200u);
    EXPECT_EQ(pe.subsystem, Subsystem::WindowsCui);
    EXPECT_TRUE(pe.is_executable());
    EXPECT_FALSE(pe.is_dll());
}

TEST_F(PeParserTest, ParsePe_ValidPe32_Sections) {
    auto data = create_minimal_pe32();
    auto result = parse_pe(data);
    
    ASSERT_TRUE(result.is_ok());
    
    const auto& pe = result.value();
    ASSERT_EQ(pe.sections.size(), 1u);
    
    const auto& text = pe.sections[0];
    EXPECT_STREQ(text.name, ".text");
    EXPECT_EQ(text.virtual_address, 0x1000u);
    EXPECT_EQ(text.virtual_size, 0x100u);
    EXPECT_TRUE(text.is_code());
    EXPECT_TRUE(text.is_executable());
    EXPECT_TRUE(text.is_readable());
    EXPECT_FALSE(text.is_writable());
}

// ============================================================================
// Full PE Parse Tests - PE64
// ============================================================================

TEST_F(PeParserTest, ParsePe_ValidPe64) {
    auto data = create_minimal_pe64();
    auto result = parse_pe(data);
    
    ASSERT_TRUE(result.is_ok()) << parse_error_message(result.error());
    
    const auto& pe = result.value();
    EXPECT_TRUE(pe.is_64bit);
    EXPECT_EQ(pe.machine, MachineType::AMD64);
    EXPECT_EQ(pe.entry_point_rva, 0x1000u);
    EXPECT_EQ(pe.image_base, 0x0000000140000000ULL);
    EXPECT_TRUE(pe.is_executable());
    EXPECT_TRUE(pe.is_large_address_aware());
    EXPECT_EQ(pe.subsystem, Subsystem::WindowsGui);
}

TEST_F(PeParserTest, ParsePe_ValidPe64_Sections) {
    auto data = create_minimal_pe64();
    auto result = parse_pe(data);
    
    ASSERT_TRUE(result.is_ok());
    
    const auto& pe = result.value();
    ASSERT_EQ(pe.sections.size(), 2u);
    
    const auto& text = pe.sections[0];
    EXPECT_STREQ(text.name, ".text");
    EXPECT_TRUE(text.is_code());
    EXPECT_TRUE(text.is_executable());
    
    const auto& ddata = pe.sections[1];
    EXPECT_STREQ(ddata.name, ".data");
    EXPECT_TRUE(ddata.is_initialized_data());
    EXPECT_TRUE(ddata.is_readable());
    EXPECT_TRUE(ddata.is_writable());
    EXPECT_FALSE(ddata.is_executable());
}

// ============================================================================
// RVA Conversion Tests
// ============================================================================

TEST_F(PeParserTest, RvaToFileOffset_ValidRva) {
    auto data = create_minimal_pe32();
    auto result = parse_pe(data);
    ASSERT_TRUE(result.is_ok());
    
    const auto& pe = result.value();
    
    // RVA 0x1000 is start of .text, file offset 0x200
    auto offset_result = pe.rva_to_file_offset(0x1000);
    ASSERT_TRUE(offset_result.is_ok());
    EXPECT_EQ(offset_result.value(), 0x200u);
    
    // RVA 0x1050 should be in .text at offset 0x250
    offset_result = pe.rva_to_file_offset(0x1050);
    ASSERT_TRUE(offset_result.is_ok());
    EXPECT_EQ(offset_result.value(), 0x250u);
}

TEST_F(PeParserTest, RvaToFileOffset_InvalidRva) {
    auto data = create_minimal_pe32();
    auto result = parse_pe(data);
    ASSERT_TRUE(result.is_ok());
    
    const auto& pe = result.value();
    
    // RVA 0x5000 is outside any section
    auto offset_result = pe.rva_to_file_offset(0x5000);
    ASSERT_TRUE(offset_result.is_err());
    EXPECT_EQ(offset_result.error(), ParseError::InvalidRva);
}

TEST_F(PeParserTest, FindSectionByRva_Found) {
    auto data = create_minimal_pe64();
    auto result = parse_pe(data);
    ASSERT_TRUE(result.is_ok());
    
    const auto& pe = result.value();
    
    // Find .text section
    const auto* section = pe.find_section_by_rva(0x1000);
    ASSERT_NE(section, nullptr);
    EXPECT_STREQ(section->name, ".text");
    
    // Find .data section
    section = pe.find_section_by_rva(0x2000);
    ASSERT_NE(section, nullptr);
    EXPECT_STREQ(section->name, ".data");
}

TEST_F(PeParserTest, FindSectionByRva_NotFound) {
    auto data = create_minimal_pe32();
    auto result = parse_pe(data);
    ASSERT_TRUE(result.is_ok());
    
    const auto& pe = result.value();
    
    // RVA 0x500 is in headers, not a section
    const auto* section = pe.find_section_by_rva(0x500);
    EXPECT_EQ(section, nullptr);
}

// ============================================================================
// Error Cases
// ============================================================================

TEST_F(PeParserTest, ParsePe_EmptyFile) {
    std::vector<u8> data;
    auto result = parse_pe(data);
    
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), ParseError::TruncatedFile);
}

TEST_F(PeParserTest, ParsePe_InvalidDosHeader) {
    std::vector<u8> data(512, 0);
    auto result = parse_pe(data);
    
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), ParseError::InvalidDosHeader);
}

TEST_F(PeParserTest, ParsePe_InvalidPeOffsetTooSmall) {
    auto data = create_minimal_pe32();
    
    // Set e_lfanew to a value smaller than DOS header size
    u32 bad_offset = 8;
    std::memcpy(data.data() + 0x3c, &bad_offset, sizeof(u32));
    
    auto result = parse_pe(data);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), ParseError::InvalidDosHeader);
}

TEST_F(PeParserTest, ParsePe_InvalidPeOffsetPastEof) {
    auto data = create_minimal_pe32();
    
    // e_lfanew past EOF — retail PEs may use large DOS stubs, but the NT
    // headers must still lie within the file.
    u32 bad_offset = static_cast<u32>(data.size() + 64);
    std::memcpy(data.data() + 0x3c, &bad_offset, sizeof(u32));
    
    auto result = parse_pe(data);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), ParseError::TruncatedFile);
}

TEST_F(PeParserTest, ParsePe_LargeDosStubOffset) {
    // RCT3.exe uses e_lfanew = 0x930; ensure we accept large-but-in-bounds stubs.
    constexpr u32 kPeOffset = 0x930;
    auto data = create_minimal_pe32();
    data.insert(data.begin() + static_cast<std::ptrdiff_t>(sizeof(DosHeader)),
                kPeOffset - sizeof(DosHeader), std::uint8_t{0});
    std::memcpy(data.data() + 0x3c, &kPeOffset, sizeof(u32));

    auto result = parse_pe(data);
    ASSERT_TRUE(result.is_ok()) << parse_error_message(result.error());
    EXPECT_FALSE(result.value().is_64bit);
}

TEST_F(PeParserTest, ParsePe_TruncatedOptionalHeader) {
    auto data = create_minimal_pe32();
    
    // Truncate file in the middle of optional header
    data.resize(sizeof(DosHeader) + 4 + sizeof(CoffHeader) + 50);
    
    auto result = parse_pe(data);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), ParseError::TruncatedFile);
}

// ============================================================================
// Error Message Tests
// ============================================================================

TEST(ParseErrorTest, ErrorMessages) {
    EXPECT_STREQ(parse_error_message(ParseError::Success), "Success");
    EXPECT_STREQ(parse_error_message(ParseError::InvalidDosHeader), 
                 "Invalid DOS header (missing MZ signature)");
    EXPECT_STREQ(parse_error_message(ParseError::TruncatedFile), 
                 "Truncated file");
}

// ============================================================================
// Structure Size Tests
// ============================================================================

TEST(PeStructuresTest, StructureSizes) {
    // Verify structures have expected sizes (already checked with static_assert)
    EXPECT_EQ(sizeof(DosHeader), 64u);
    EXPECT_EQ(sizeof(CoffHeader), 20u);
    EXPECT_EQ(sizeof(SectionHeader), 40u);
    EXPECT_EQ(sizeof(DataDirectory), 8u);
}
