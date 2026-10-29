// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// PE file parser implementation

#include "pe_parser.hpp"
#include <cstring>
#include <algorithm>

namespace vitreo::pe {

// ============================================================================
// Error Messages
// ============================================================================

const char* parse_error_message(ParseError err) noexcept {
    switch (err) {
        case ParseError::Success:
            return "Success";
        case ParseError::InvalidDosHeader:
            return "Invalid DOS header (missing MZ signature)";
        case ParseError::InvalidPeSignature:
            return "Invalid PE signature (missing PE\\0\\0)";
        case ParseError::InvalidCoffHeader:
            return "Invalid COFF header";
        case ParseError::InvalidOptionalHeader:
            return "Invalid optional header";
        case ParseError::UnsupportedMachine:
            return "Unsupported machine type";
        case ParseError::InvalidSectionHeader:
            return "Invalid section header";
        case ParseError::TruncatedFile:
            return "Truncated file";
        case ParseError::InvalidRva:
            return "Invalid RVA";
    }
    return "Unknown error";
}

// ============================================================================
// Helper Functions
// ============================================================================

namespace {

/// Safely read a value from a byte span
template<typename T>
[[nodiscard]] bool safe_read(std::span<const u8> data, size_t offset, T& out) noexcept {
    if (offset + sizeof(T) > data.size()) {
        return false;
    }
    std::memcpy(&out, data.data() + offset, sizeof(T));
    return true;
}

/// Check if machine type is supported (x86 or x64)
[[nodiscard]] bool is_supported_machine(u16 machine) noexcept {
    return machine == static_cast<u16>(MachineType::I386) || 
           machine == static_cast<u16>(MachineType::AMD64);
}

} // anonymous namespace

// ============================================================================
// ParsedPe Methods
// ============================================================================

const ParsedSection* ParsedPe::find_section_by_rva(u32 rva) const noexcept {
    for (const auto& section : sections) {
        u32 section_end = section.virtual_address + section.virtual_size;
        if (rva >= section.virtual_address && rva < section_end) {
            return &section;
        }
    }
    return nullptr;
}

Result<u32, ParseError> ParsedPe::rva_to_file_offset(u32 rva) const noexcept {
    const ParsedSection* section = find_section_by_rva(rva);
    if (!section) {
        return Result<u32, ParseError>::err(ParseError::InvalidRva);
    }
    
    u32 offset_within_section = rva - section->virtual_address;
    if (offset_within_section >= section->raw_size) {
        // RVA points to uninitialized data or padding
        return Result<u32, ParseError>::err(ParseError::InvalidRva);
    }
    
    return Result<u32, ParseError>::ok(section->raw_offset + offset_within_section);
}

// ============================================================================
// Individual Parsing Functions
// ============================================================================

Result<DosHeader, ParseError> parse_dos_header(std::span<const u8> data) noexcept {
    if (data.size() < sizeof(DosHeader)) {
        return Result<DosHeader, ParseError>::err(ParseError::TruncatedFile);
    }
    
    DosHeader dos{};
    std::memcpy(&dos, data.data(), sizeof(DosHeader));
    
    // Check MZ signature
    if (dos.e_magic != DOS_MAGIC) {
        return Result<DosHeader, ParseError>::err(ParseError::InvalidDosHeader);
    }
    
    return Result<DosHeader, ParseError>::ok(dos);
}

Result<void, ParseError> validate_pe_signature(
    std::span<const u8> data,
    u32 offset
) noexcept {
    if (offset + 4 > data.size()) {
        return Result<void, ParseError>::err(ParseError::TruncatedFile);
    }
    
    u32 signature{};
    std::memcpy(&signature, data.data() + offset, sizeof(u32));
    
    if (signature != PE_SIGNATURE) {
        return Result<void, ParseError>::err(ParseError::InvalidPeSignature);
    }
    
    return Result<void, ParseError>::ok();
}

Result<CoffHeader, ParseError> parse_coff_header(
    std::span<const u8> data,
    u32 offset
) noexcept {
    if (offset + sizeof(CoffHeader) > data.size()) {
        return Result<CoffHeader, ParseError>::err(ParseError::TruncatedFile);
    }
    
    CoffHeader coff{};
    std::memcpy(&coff, data.data() + offset, sizeof(CoffHeader));
    
    // Validate machine type
    if (!is_supported_machine(coff.Machine)) {
        return Result<CoffHeader, ParseError>::err(ParseError::UnsupportedMachine);
    }
    
    return Result<CoffHeader, ParseError>::ok(coff);
}

// ============================================================================
// Main Parsing Function
// ============================================================================

Result<ParsedPe, ParseError> parse_pe(std::span<const u8> data) noexcept {
    ParsedPe parsed{};
    
    // --- Parse DOS Header ---
    auto dos_result = parse_dos_header(data);
    if (dos_result.is_err()) {
        return Result<ParsedPe, ParseError>::err(dos_result.error());
    }
    DosHeader dos = dos_result.value();
    
    // Older retail binaries place a large DOS stub, so e_lfanew is often
    // well above 1 KiB. The PE/COFF contract only requires that the NT
    // headers lie within the file. Reject offsets that overlap the DOS
    // header itself or that cannot hold a PE signature.
    if (dos.e_lfanew < sizeof(DosHeader) ||
        static_cast<size_t>(dos.e_lfanew) + 4 > data.size()) {
        return Result<ParsedPe, ParseError>::err(
            dos.e_lfanew < sizeof(DosHeader)
                ? ParseError::InvalidDosHeader
                : ParseError::TruncatedFile);
    }
    
    // --- Validate PE Signature ---
    auto sig_result = validate_pe_signature(data, dos.e_lfanew);
    if (sig_result.is_err()) {
        return Result<ParsedPe, ParseError>::err(sig_result.error());
    }
    
    // --- Parse COFF Header ---
    u32 coff_offset = dos.e_lfanew + 4; // Skip PE signature
    auto coff_result = parse_coff_header(data, coff_offset);
    if (coff_result.is_err()) {
        return Result<ParsedPe, ParseError>::err(coff_result.error());
    }
    CoffHeader coff = coff_result.value();
    
    parsed.machine = static_cast<MachineType>(coff.Machine);
    parsed.characteristics = coff.Characteristics;
    
    // Determine if 32-bit or 64-bit
    u32 optional_offset = coff_offset + sizeof(CoffHeader);
    if (optional_offset + 2 > data.size()) {
        return Result<ParsedPe, ParseError>::err(ParseError::TruncatedFile);
    }
    
    u16 magic{};
    std::memcpy(&magic, data.data() + optional_offset, sizeof(u16));
    
    // --- Parse Optional Header ---
    if (magic == PE32_MAGIC) {
        parsed.is_64bit = false;
        
        if (optional_offset + sizeof(OptionalHeader32) > data.size()) {
            return Result<ParsedPe, ParseError>::err(ParseError::TruncatedFile);
        }
        
        OptionalHeader32 opt{};
        std::memcpy(&opt, data.data() + optional_offset, sizeof(OptionalHeader32));
        
        if (opt.Magic != PE32_MAGIC) {
            return Result<ParsedPe, ParseError>::err(ParseError::InvalidOptionalHeader);
        }
        
        parsed.image_base = opt.ImageBase;
        parsed.entry_point_rva = opt.AddressOfEntryPoint;
        parsed.section_alignment = opt.SectionAlignment;
        parsed.file_alignment = opt.FileAlignment;
        parsed.size_of_image = opt.SizeOfImage;
        parsed.size_of_headers = opt.SizeOfHeaders;
        parsed.subsystem = static_cast<Subsystem>(opt.Subsystem);
        parsed.major_subsystem_version = opt.MajorSubsystemVersion;
        parsed.minor_subsystem_version = opt.MinorSubsystemVersion;
        parsed.dll_characteristics = opt.DllCharacteristics;
        parsed.stack_reserve = opt.SizeOfStackReserve;
        parsed.stack_commit = opt.SizeOfStackCommit;
        parsed.heap_reserve = opt.SizeOfHeapReserve;
        parsed.heap_commit = opt.SizeOfHeapCommit;
        parsed.num_data_directories = std::min<u32>(opt.NumberOfRvaAndSizes, DIR_COUNT);
        
        // Copy data directories
        for (u32 i = 0; i < parsed.num_data_directories; ++i) {
            parsed.data_directories[i] = opt.DataDirectory[i];
        }
        
    } else if (magic == PE32PLUS_MAGIC) {
        parsed.is_64bit = true;
        
        if (optional_offset + sizeof(OptionalHeader64) > data.size()) {
            return Result<ParsedPe, ParseError>::err(ParseError::TruncatedFile);
        }
        
        OptionalHeader64 opt{};
        std::memcpy(&opt, data.data() + optional_offset, sizeof(OptionalHeader64));
        
        if (opt.Magic != PE32PLUS_MAGIC) {
            return Result<ParsedPe, ParseError>::err(ParseError::InvalidOptionalHeader);
        }
        
        parsed.image_base = opt.ImageBase;
        parsed.entry_point_rva = opt.AddressOfEntryPoint;
        parsed.section_alignment = opt.SectionAlignment;
        parsed.file_alignment = opt.FileAlignment;
        parsed.size_of_image = opt.SizeOfImage;
        parsed.size_of_headers = opt.SizeOfHeaders;
        parsed.subsystem = static_cast<Subsystem>(opt.Subsystem);
        parsed.major_subsystem_version = opt.MajorSubsystemVersion;
        parsed.minor_subsystem_version = opt.MinorSubsystemVersion;
        parsed.dll_characteristics = opt.DllCharacteristics;
        parsed.stack_reserve = opt.SizeOfStackReserve;
        parsed.stack_commit = opt.SizeOfStackCommit;
        parsed.heap_reserve = opt.SizeOfHeapReserve;
        parsed.heap_commit = opt.SizeOfHeapCommit;
        parsed.num_data_directories = std::min<u32>(opt.NumberOfRvaAndSizes, DIR_COUNT);
        
        // Copy data directories
        for (u32 i = 0; i < parsed.num_data_directories; ++i) {
            parsed.data_directories[i] = opt.DataDirectory[i];
        }
        
    } else {
        return Result<ParsedPe, ParseError>::err(ParseError::InvalidOptionalHeader);
    }
    
    // --- Parse Section Headers ---
    u32 section_offset = optional_offset + coff.SizeOfOptionalHeader;
    
    if (section_offset + (coff.NumberOfSections * sizeof(SectionHeader)) > data.size()) {
        return Result<ParsedPe, ParseError>::err(ParseError::TruncatedFile);
    }
    
    parsed.sections.reserve(coff.NumberOfSections);
    
    for (u16 i = 0; i < coff.NumberOfSections; ++i) {
        SectionHeader sh{};
        std::memcpy(&sh, data.data() + section_offset + (i * sizeof(SectionHeader)), sizeof(SectionHeader));
        
        ParsedSection ps{};
        
        // Copy name (may not be null-terminated in header)
        std::memcpy(ps.name, sh.Name, 8);
        ps.name[8] = '\0';
        
        ps.virtual_address = sh.VirtualAddress;
        ps.virtual_size = sh.VirtualSize;
        ps.raw_offset = sh.PointerToRawData;
        ps.raw_size = sh.SizeOfRawData;
        ps.characteristics = sh.Characteristics;
        
        // Basic validation
        if (ps.raw_offset != 0 && ps.raw_offset + ps.raw_size > data.size()) {
            return Result<ParsedPe, ParseError>::err(ParseError::InvalidSectionHeader);
        }
        
        parsed.sections.push_back(ps);
    }
    
    return Result<ParsedPe, ParseError>::ok(std::move(parsed));
}

} // namespace vitreo::pe
