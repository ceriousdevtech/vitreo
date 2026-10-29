// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// PE section mapper

#pragma once

#include "../parser/pe_parser.hpp"
#include "core/memory/guest_memory.hpp"
#include <vitreo/result.hpp>
#include <span>

namespace vitreo::pe {

// ============================================================================
// Mapping Errors
// ============================================================================

/// Section mapping error codes
enum class MapError : u8 {
    Success = 0,
    InvalidPe,
    InvalidSection,
    MemoryMapFailed,
    ProtectionFailed,
    SectionOverlap,
    ImageTooLarge,
    AddressOverflow,
};

/// Get human-readable error message
[[nodiscard]] const char* map_error_message(MapError err) noexcept;

// ============================================================================
// Mapped Image Information
// ============================================================================

/// Information about a mapped section
struct MappedSection {
    u64 guest_address;          ///< Guest virtual address
    u32 size;                   ///< Size in memory
    MemoryProtection protection; ///< Host MemoryProtection
    /// Win32 PAGE_* as Windows reports for an unwritten image page of this
    /// section (WRITECOPY forms for writable sections). VirtualQuery /
    /// VirtualProtect old-protect use this, not native_prot_to_win32.
    u32 win32_protection{0};
    char name[9];               ///< Section name
};

/// Result of mapping a PE into memory
struct MappedImage {
    u64 base_address;           ///< Base address of loaded image
    u64 entry_point;            ///< Address of entry point
    u32 image_size;             ///< Total image size in memory
    /// Bytes from `base_address` that hold the PE headers. Windows maps this
    /// span PAGE_READONLY (not copy-on-write writable). Packers such as Petite
    /// intentionally store through the image base to raise ACCESS_VIOLATION and
    /// enter their SEH unpack path — so Vitreo must not leave headers writable.
    u32 header_size = 0;
    std::vector<MappedSection> sections; ///< Mapped sections
    
    /// Check if the image has an entry point
    [[nodiscard]] bool has_entry_point() const noexcept {
        return entry_point != 0;
    }
};

// ============================================================================
// Section Mapper
// ============================================================================

/// Maps PE sections into guest memory
class SectionMapper {
public:
    /// Create a mapper for the given guest memory
    explicit SectionMapper(GuestMemory& memory) noexcept
        : m_memory(memory) {}
    
    /// Map a PE file into guest memory
    /// @param pe Parsed PE information
    /// @param pe_data Raw PE file data
    /// @param load_address Optional load address (0 = use PE's ImageBase)
    /// @return Mapped image info or error
    [[nodiscard]] Result<MappedImage, MapError> map(
        const ParsedPe& pe,
        std::span<const u8> pe_data,
        u64 load_address = 0
    ) noexcept;
    
    /// Unmap a previously mapped image
    /// @param image The mapped image to unmap
    /// @return Success or error
    [[nodiscard]] Result<void, MapError> unmap(const MappedImage& image) noexcept;

private:
    /// Convert PE section characteristics to memory protection
    [[nodiscard]] static MemoryProtection section_to_protection(u32 characteristics) noexcept;

    GuestMemory& m_memory;
};

// ============================================================================
// Convenience Functions
// ============================================================================

/// Map a PE file into freshly-allocated guest memory
/// @param memory Guest memory to map into
/// @param pe Parsed PE information
/// @param pe_data Raw PE file data
/// @param load_address Optional load address (0 = use PE's ImageBase)
/// @return Mapped image info or error
[[nodiscard]] Result<MappedImage, MapError> map_pe(
    GuestMemory& memory,
    const ParsedPe& pe,
    std::span<const u8> pe_data,
    u64 load_address = 0
) noexcept;

} // namespace vitreo::pe
