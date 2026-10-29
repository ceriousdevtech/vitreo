// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// PE file parser

#pragma once

#include "pe_structures.hpp"
#include <vitreo/result.hpp>
#include <span>
#include <string_view>
#include <vector>

namespace vitreo::pe {

// ============================================================================
// Parse Errors
// ============================================================================

/// PE parsing error codes
enum class ParseError : u8 {
    Success = 0,
    InvalidDosHeader,
    InvalidPeSignature,
    InvalidCoffHeader,
    InvalidOptionalHeader,
    UnsupportedMachine,
    InvalidSectionHeader,
    TruncatedFile,
    InvalidRva,
};

/// Get human-readable error message
[[nodiscard]] const char* parse_error_message(ParseError err) noexcept;

// ============================================================================
// Parsed PE Information
// ============================================================================

/// Parsed section information
struct ParsedSection {
    char name[9];           ///< Null-terminated section name
    u32 virtual_address;    ///< RVA
    u32 virtual_size;       ///< Size when loaded
    u32 raw_offset;         ///< File offset
    u32 raw_size;           ///< Size on disk
    u32 characteristics;    ///< Section flags
    
    [[nodiscard]] bool is_code() const noexcept {
        return (characteristics & SCN_CNT_CODE) != 0;
    }
    
    [[nodiscard]] bool is_initialized_data() const noexcept {
        return (characteristics & SCN_CNT_INITIALIZED_DATA) != 0;
    }
    
    [[nodiscard]] bool is_uninitialized_data() const noexcept {
        return (characteristics & SCN_CNT_UNINITIALIZED_DATA) != 0;
    }
    
    [[nodiscard]] bool is_readable() const noexcept {
        return (characteristics & SCN_MEM_READ) != 0;
    }
    
    [[nodiscard]] bool is_writable() const noexcept {
        return (characteristics & SCN_MEM_WRITE) != 0;
    }
    
    [[nodiscard]] bool is_executable() const noexcept {
        return (characteristics & SCN_MEM_EXECUTE) != 0;
    }
};

/// Parsed PE file information
struct ParsedPe {
    // Basic info
    bool is_64bit;
    MachineType machine;
    u16 characteristics;      ///< COFF characteristics
    
    // Addresses and sizes
    u64 image_base;
    u32 entry_point_rva;
    u32 section_alignment;
    u32 file_alignment;
    u32 size_of_image;
    u32 size_of_headers;
    
    // Subsystem
    Subsystem subsystem;
    /// IMAGE_OPTIONAL_HEADER Major/MinorSubsystemVersion. USER keys some
    /// compatibility metrics on an image older than 6.0 (Vista).
    u16 major_subsystem_version = 0;
    u16 minor_subsystem_version = 0;
    u16 dll_characteristics;
    
    // Stack and heap
    u64 stack_reserve;
    u64 stack_commit;
    u64 heap_reserve;
    u64 heap_commit;
    
    // Data directories
    DataDirectory data_directories[DIR_COUNT];
    u32 num_data_directories;
    
    // Sections
    std::vector<ParsedSection> sections;
    
    // Helper methods
    /// True when this image carries a CLR runtime header, i.e. it is a
    /// managed (.NET) assembly rather than a native image.
    [[nodiscard]] bool is_managed() const noexcept {
        return data_directories[DIR_CLR_RUNTIME].VirtualAddress != 0 &&
               data_directories[DIR_CLR_RUNTIME].Size != 0;
    }

    [[nodiscard]] bool is_dll() const noexcept {
        return (characteristics & CoffCharacteristics::DLL) != 0;
    }
    
    [[nodiscard]] bool is_executable() const noexcept {
        return (characteristics & CoffCharacteristics::EXECUTABLE_IMAGE) != 0;
    }
    
    [[nodiscard]] bool is_large_address_aware() const noexcept {
        return (characteristics & CoffCharacteristics::LARGE_ADDRESS_AWARE) != 0;
    }
    
    [[nodiscard]] bool has_relocations() const noexcept {
        return data_directories[DIR_BASERELOC].VirtualAddress != 0 &&
               data_directories[DIR_BASERELOC].Size != 0;
    }
    
    [[nodiscard]] bool has_imports() const noexcept {
        return data_directories[DIR_IMPORT].VirtualAddress != 0 &&
               data_directories[DIR_IMPORT].Size != 0;
    }
    
    [[nodiscard]] bool has_exports() const noexcept {
        return data_directories[DIR_EXPORT].VirtualAddress != 0 &&
               data_directories[DIR_EXPORT].Size != 0;
    }
    
    /// Find section containing an RVA
    [[nodiscard]] const ParsedSection* find_section_by_rva(u32 rva) const noexcept;
    
    /// Convert RVA to file offset
    [[nodiscard]] Result<u32, ParseError> rva_to_file_offset(u32 rva) const noexcept;
};

// ============================================================================
// PE Parser
// ============================================================================

/// Parse a PE file from memory
/// @param data Raw PE file data
/// @return Parsed PE information or error
[[nodiscard]] Result<ParsedPe, ParseError> parse_pe(std::span<const u8> data) noexcept;

/// Parse just the DOS header
[[nodiscard]] Result<DosHeader, ParseError> parse_dos_header(std::span<const u8> data) noexcept;

/// Validate PE signature at given offset
[[nodiscard]] Result<void, ParseError> validate_pe_signature(
    std::span<const u8> data, 
    u32 offset
) noexcept;

/// Parse COFF header
[[nodiscard]] Result<CoffHeader, ParseError> parse_coff_header(
    std::span<const u8> data,
    u32 offset
) noexcept;

} // namespace vitreo::pe
