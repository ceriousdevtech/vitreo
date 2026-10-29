// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Relocator unit tests

#include <gtest/gtest.h>
#include <core/pe/reloc/relocator.hpp>
#include <core/pe/mapper/section_mapper.hpp>
#include <core/pe/parser/pe_parser.hpp>
#include <core/memory/guest_memory.hpp>
#include <cstring>

using namespace vitreo;
using namespace vitreo::pe;

// ============================================================================
// Test Fixture
// ============================================================================

class RelocatorTest : public ::testing::Test {
protected:
    GuestMemory memory;
    
    // Build a minimal valid PE32 file with relocation data
    std::vector<u8> create_test_pe_with_relocs() {
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
        coff.NumberOfSections = 2;
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
        opt.SizeOfImage = 0x3000;
        opt.SizeOfHeaders = 0x400;
        opt.Subsystem = static_cast<u16>(Subsystem::WindowsCui);
        opt.NumberOfRvaAndSizes = 16;
        
        // Set up base relocation data directory
        // .reloc section will be at RVA 0x2000, file offset 0x600
        opt.DataDirectory[DIR_BASERELOC].VirtualAddress = 0x2000;
        opt.DataDirectory[DIR_BASERELOC].Size = 0x14;  // One block with 6 entries
        
        size_t opt_offset = data.size();
        data.resize(data.size() + sizeof(OptionalHeader32));
        std::memcpy(data.data() + opt_offset, &opt, sizeof(OptionalHeader32));
        
        // Section 1: .data (data with addresses to relocate)
        SectionHeader sec1{};
        std::memcpy(sec1.Name, ".data\0\0\0", 8);
        sec1.VirtualSize = 0x100;
        sec1.VirtualAddress = 0x1000;
        sec1.SizeOfRawData = 0x200;
        sec1.PointerToRawData = 0x400;
        sec1.Characteristics = SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ | SCN_MEM_WRITE;
        
        size_t sec1_offset = data.size();
        data.resize(data.size() + sizeof(SectionHeader));
        std::memcpy(data.data() + sec1_offset, &sec1, sizeof(SectionHeader));
        
        // Section 2: .reloc (relocation data)
        SectionHeader sec2{};
        std::memcpy(sec2.Name, ".reloc\0\0", 8);
        sec2.VirtualSize = 0x100;
        sec2.VirtualAddress = 0x2000;
        sec2.SizeOfRawData = 0x200;
        sec2.PointerToRawData = 0x600;
        sec2.Characteristics = SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ | SCN_MEM_DISCARDABLE;
        
        size_t sec2_offset = data.size();
        data.resize(data.size() + sizeof(SectionHeader));
        std::memcpy(data.data() + sec2_offset, &sec2, sizeof(SectionHeader));
        
        // Pad to headers end (0x400)
        data.resize(0x400, 0);
        
        // Section 1 data (.data) - addresses that need relocation
        data.resize(0x600, 0);
        
        // At offset 0x00: 32-bit address 0x00401000 (needs relocation)
        u32 addr1 = 0x00401000;
        std::memcpy(data.data() + 0x400, &addr1, sizeof(addr1));
        
        // At offset 0x10: 32-bit address 0x00402000 (needs relocation)
        u32 addr2 = 0x00402000;
        std::memcpy(data.data() + 0x410, &addr2, sizeof(addr2));
        
        // At offset 0x20: 32-bit address 0x00403000 (needs relocation)
        u32 addr3 = 0x00403000;
        std::memcpy(data.data() + 0x420, &addr3, sizeof(addr3));
        
        // Section 2 data (.reloc) - relocation block
        data.resize(0x800, 0);
        
        // Base relocation block for page at RVA 0x1000
        // Format: page_rva (4 bytes), block_size (4 bytes), entries (2 bytes each)
        BaseRelocBlock block{};
        block.page_rva = 0x1000;
        block.block_size = 8 + 6 * 2;  // Header + 6 entries (including padding)
        std::memcpy(data.data() + 0x600, &block, sizeof(block));
        
        // Relocation entries (high 4 bits = type, low 12 bits = offset)
        // Type 3 (HIGHLOW) at offset 0x00
        u16 entry1 = (3 << 12) | 0x000;
        std::memcpy(data.data() + 0x608, &entry1, sizeof(entry1));
        
        // Type 3 (HIGHLOW) at offset 0x10
        u16 entry2 = (3 << 12) | 0x010;
        std::memcpy(data.data() + 0x60A, &entry2, sizeof(entry2));
        
        // Type 3 (HIGHLOW) at offset 0x20
        u16 entry3 = (3 << 12) | 0x020;
        std::memcpy(data.data() + 0x60C, &entry3, sizeof(entry3));
        
        // Type 0 (ABSOLUTE) for padding
        u16 entry4 = 0;
        std::memcpy(data.data() + 0x60E, &entry4, sizeof(entry4));
        std::memcpy(data.data() + 0x610, &entry4, sizeof(entry4));
        std::memcpy(data.data() + 0x612, &entry4, sizeof(entry4));
        
        return data;
    }
    
    // Build PE64 with 64-bit relocations
    std::vector<u8> create_test_pe64_with_relocs() {
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
        coff.Characteristics = CoffCharacteristics::EXECUTABLE_IMAGE | CoffCharacteristics::LARGE_ADDRESS_AWARE;
        
        size_t coff_offset = data.size();
        data.resize(data.size() + sizeof(CoffHeader));
        std::memcpy(data.data() + coff_offset, &coff, sizeof(CoffHeader));
        
        // Optional Header (PE32+)
        OptionalHeader64 opt{};
        opt.Magic = PE32PLUS_MAGIC;
        opt.AddressOfEntryPoint = 0x1000;
        opt.ImageBase = 0x0000000140000000ULL;
        opt.SectionAlignment = 0x1000;
        opt.FileAlignment = 0x200;
        opt.MajorOperatingSystemVersion = 6;
        opt.MinorOperatingSystemVersion = 0;
        opt.SizeOfImage = 0x3000;
        opt.SizeOfHeaders = 0x400;
        opt.Subsystem = static_cast<u16>(Subsystem::WindowsCui);
        opt.NumberOfRvaAndSizes = 16;
        
        // .reloc at RVA 0x2000
        opt.DataDirectory[DIR_BASERELOC].VirtualAddress = 0x2000;
        opt.DataDirectory[DIR_BASERELOC].Size = 0x10;  // One block with 4 entries
        
        size_t opt_offset = data.size();
        data.resize(data.size() + sizeof(OptionalHeader64));
        std::memcpy(data.data() + opt_offset, &opt, sizeof(OptionalHeader64));
        
        // Section 1: .data
        SectionHeader sec1{};
        std::memcpy(sec1.Name, ".data\0\0\0", 8);
        sec1.VirtualSize = 0x100;
        sec1.VirtualAddress = 0x1000;
        sec1.SizeOfRawData = 0x200;
        sec1.PointerToRawData = 0x400;
        sec1.Characteristics = SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ | SCN_MEM_WRITE;
        
        size_t sec1_offset = data.size();
        data.resize(data.size() + sizeof(SectionHeader));
        std::memcpy(data.data() + sec1_offset, &sec1, sizeof(SectionHeader));
        
        // Section 2: .reloc
        SectionHeader sec2{};
        std::memcpy(sec2.Name, ".reloc\0\0", 8);
        sec2.VirtualSize = 0x100;
        sec2.VirtualAddress = 0x2000;
        sec2.SizeOfRawData = 0x200;
        sec2.PointerToRawData = 0x600;
        sec2.Characteristics = SCN_CNT_INITIALIZED_DATA | SCN_MEM_READ | SCN_MEM_DISCARDABLE;
        
        size_t sec2_offset = data.size();
        data.resize(data.size() + sizeof(SectionHeader));
        std::memcpy(data.data() + sec2_offset, &sec2, sizeof(SectionHeader));
        
        // Pad to headers end (0x400)
        data.resize(0x400, 0);
        
        // Section 1 data (.data) - 64-bit addresses
        data.resize(0x600, 0);
        
        // At offset 0x00: 64-bit address
        u64 addr1 = 0x0000000140001000ULL;
        std::memcpy(data.data() + 0x400, &addr1, sizeof(addr1));
        
        // At offset 0x10: 64-bit address
        u64 addr2 = 0x0000000140002000ULL;
        std::memcpy(data.data() + 0x410, &addr2, sizeof(addr2));
        
        // Section 2 data (.reloc)
        data.resize(0x800, 0);
        
        // Relocation block for page at RVA 0x1000
        BaseRelocBlock block{};
        block.page_rva = 0x1000;
        block.block_size = 8 + 4 * 2;  // Header + 4 entries
        std::memcpy(data.data() + 0x600, &block, sizeof(block));
        
        // Type 10 (DIR64) at offset 0x00
        u16 entry1 = (10 << 12) | 0x000;
        std::memcpy(data.data() + 0x608, &entry1, sizeof(entry1));
        
        // Type 10 (DIR64) at offset 0x10
        u16 entry2 = (10 << 12) | 0x010;
        std::memcpy(data.data() + 0x60A, &entry2, sizeof(entry2));
        
        // Padding
        u16 pad = 0;
        std::memcpy(data.data() + 0x60C, &pad, sizeof(pad));
        std::memcpy(data.data() + 0x60E, &pad, sizeof(pad));
        
        return data;
    }
};

// ============================================================================
// Parse Entry Tests
// ============================================================================

TEST_F(RelocatorTest, ParseEntry_HighLow) {
    u16 value = (3 << 12) | 0x100;  // Type 3, offset 0x100
    auto entry = Relocator::parse_entry(value);
    
    EXPECT_EQ(entry.type, RelocType::HighLow);
    EXPECT_EQ(entry.offset, 0x100u);
}

TEST_F(RelocatorTest, ParseEntry_Dir64) {
    u16 value = (10 << 12) | 0x050;  // Type 10, offset 0x50
    auto entry = Relocator::parse_entry(value);
    
    EXPECT_EQ(entry.type, RelocType::Dir64);
    EXPECT_EQ(entry.offset, 0x050u);
}

TEST_F(RelocatorTest, ParseEntry_Absolute) {
    u16 value = 0;  // Type 0, offset 0
    auto entry = Relocator::parse_entry(value);
    
    EXPECT_EQ(entry.type, RelocType::Absolute);
    EXPECT_EQ(entry.offset, 0u);
}

// ============================================================================
// Needs Relocation Tests
// ============================================================================

TEST_F(RelocatorTest, NeedsRelocation_SameBase) {
    auto pe_data = create_test_pe_with_relocs();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    const auto& pe = parse_result.value();
    
    // Same base = no relocation needed
    EXPECT_FALSE(Relocator::needs_relocation(pe, pe.image_base));
}

TEST_F(RelocatorTest, NeedsRelocation_DifferentBase) {
    auto pe_data = create_test_pe_with_relocs();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    const auto& pe = parse_result.value();
    
    // Different base = relocation needed
    EXPECT_TRUE(Relocator::needs_relocation(pe, pe.image_base + 0x10000));
}

// ============================================================================
// 32-bit Relocation Tests
// ============================================================================

TEST_F(RelocatorTest, Apply_32bitRelocations) {
    auto pe_data = create_test_pe_with_relocs();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    const auto& pe = parse_result.value();
    
    // Map PE at a different address
    u64 new_base = 0x00500000;
    auto map_result = map_pe(memory, pe, pe_data, new_base);
    ASSERT_TRUE(map_result.is_ok()) << map_error_message(map_result.error());
    
    // Apply relocations
    Relocator relocator(memory);
    auto reloc_result = relocator.apply(pe, pe_data, new_base);
    ASSERT_TRUE(reloc_result.is_ok()) << reloc_error_message(reloc_result.error());
    
    const auto& stats = reloc_result.value();
    EXPECT_EQ(stats.delta, 0x00100000);  // 0x500000 - 0x400000
    EXPECT_EQ(stats.blocks_processed, 1u);
    EXPECT_EQ(stats.entries_applied, 3u);
    
    // Verify relocated addresses
    // Original 0x00401000 -> 0x00501000
    auto val1 = memory.read_u32(new_base + 0x1000);
    ASSERT_TRUE(val1.is_ok());
    EXPECT_EQ(val1.value(), 0x00501000u);
    
    // Original 0x00402000 -> 0x00502000
    auto val2 = memory.read_u32(new_base + 0x1010);
    ASSERT_TRUE(val2.is_ok());
    EXPECT_EQ(val2.value(), 0x00502000u);
    
    // Original 0x00403000 -> 0x00503000
    auto val3 = memory.read_u32(new_base + 0x1020);
    ASSERT_TRUE(val3.is_ok());
    EXPECT_EQ(val3.value(), 0x00503000u);
}

TEST_F(RelocatorTest, Apply_NoDeltaNoOp) {
    auto pe_data = create_test_pe_with_relocs();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    const auto& pe = parse_result.value();
    
    // Map PE at preferred base
    auto map_result = map_pe(memory, pe, pe_data, pe.image_base);
    ASSERT_TRUE(map_result.is_ok());
    
    // Apply relocations (should be no-op)
    Relocator relocator(memory);
    auto reloc_result = relocator.apply(pe, pe_data, pe.image_base);
    ASSERT_TRUE(reloc_result.is_ok());
    
    const auto& stats = reloc_result.value();
    EXPECT_EQ(stats.delta, 0);
    EXPECT_EQ(stats.blocks_processed, 0u);
    EXPECT_EQ(stats.entries_applied, 0u);
}

TEST_F(RelocatorTest, Apply_NegativeDelta) {
    auto pe_data = create_test_pe_with_relocs();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    const auto& pe = parse_result.value();
    
    // Map PE at lower address
    u64 new_base = 0x00300000;
    auto map_result = map_pe(memory, pe, pe_data, new_base);
    ASSERT_TRUE(map_result.is_ok());
    
    // Apply relocations with negative delta
    Relocator relocator(memory);
    auto reloc_result = relocator.apply(pe, pe_data, new_base);
    ASSERT_TRUE(reloc_result.is_ok());
    
    const auto& stats = reloc_result.value();
    EXPECT_EQ(stats.delta, -0x00100000);  // 0x300000 - 0x400000
    
    // Verify relocated addresses
    auto val1 = memory.read_u32(new_base + 0x1000);
    ASSERT_TRUE(val1.is_ok());
    EXPECT_EQ(val1.value(), 0x00301000u);
}

// ============================================================================
// 64-bit Relocation Tests
// ============================================================================

TEST_F(RelocatorTest, Apply_64bitRelocations) {
    auto pe_data = create_test_pe64_with_relocs();
    auto parse_result = parse_pe(pe_data);
    ASSERT_TRUE(parse_result.is_ok());
    
    const auto& pe = parse_result.value();
    EXPECT_TRUE(pe.is_64bit);
    
    // Map PE at a different address
    u64 new_base = 0x0000000150000000ULL;
    auto map_result = map_pe(memory, pe, pe_data, new_base);
    ASSERT_TRUE(map_result.is_ok()) << map_error_message(map_result.error());
    
    // Apply relocations
    auto reloc_result = apply_relocations(memory, pe, pe_data, new_base);
    ASSERT_TRUE(reloc_result.is_ok()) << reloc_error_message(reloc_result.error());
    
    const auto& stats = reloc_result.value();
    EXPECT_EQ(stats.delta, 0x10000000);  // 0x150000000 - 0x140000000
    EXPECT_EQ(stats.entries_applied, 2u);
    
    // Verify relocated 64-bit addresses
    auto val1 = memory.read_u64(new_base + 0x1000);
    ASSERT_TRUE(val1.is_ok());
    EXPECT_EQ(val1.value(), 0x0000000150001000ULL);
    
    auto val2 = memory.read_u64(new_base + 0x1010);
    ASSERT_TRUE(val2.is_ok());
    EXPECT_EQ(val2.value(), 0x0000000150002000ULL);
}

// ============================================================================
// Error Message Tests
// ============================================================================

TEST(RelocErrorTest, ErrorMessages) {
    EXPECT_STREQ(reloc_error_message(RelocError::Success), "Success");
    EXPECT_STREQ(reloc_error_message(RelocError::NoRelocations), "No relocations present");
    EXPECT_STREQ(reloc_error_message(RelocError::InvalidRelocBlock), "Invalid relocation block");
    EXPECT_STREQ(reloc_error_message(RelocError::UnsupportedRelocType), "Unsupported relocation type");
}

// ============================================================================
// BaseRelocBlock Tests
// ============================================================================

TEST(BaseRelocBlockTest, EntryCount) {
    BaseRelocBlock block{};
    block.page_rva = 0x1000;
    block.block_size = 8 + 6 * 2;  // Header + 6 entries
    
    EXPECT_EQ(block.entry_count(), 6u);
}

TEST(BaseRelocBlockTest, EntryCount_InvalidSize) {
    BaseRelocBlock block{};
    block.page_rva = 0x1000;
    block.block_size = 4;  // Too small
    
    EXPECT_EQ(block.entry_count(), 0u);
}
