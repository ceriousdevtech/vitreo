// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// PE base relocation processor

#pragma once

#include "../parser/pe_parser.hpp"
#include "core/memory/guest_memory.hpp"
#include <vitreo/result.hpp>
#include <span>

namespace vitreo::pe {

// ============================================================================
// Relocation Errors
// ============================================================================

/// Relocation processing error codes
enum class RelocError : u8 {
    Success = 0,
    NoRelocations,
    InvalidRelocBlock,
    InvalidRelocEntry,
    MemoryReadFailed,
    MemoryWriteFailed,
    UnsupportedRelocType,
    AddressOverflow,
};

/// Get human-readable error message
[[nodiscard]] const char* reloc_error_message(RelocError err) noexcept;

// ============================================================================
// Relocation Types
// ============================================================================

/// PE base relocation types
enum class RelocType : u8 {
    Absolute    = 0,    ///< No operation (padding)
    High        = 1,    ///< High 16 bits of 32-bit value
    Low         = 2,    ///< Low 16 bits of 32-bit value
    HighLow     = 3,    ///< Full 32-bit value
    HighAdj     = 4,    ///< High 16 bits + low 16 from next entry
    MipsJmpAddr = 5,    ///< MIPS jump address
    ArmMov32    = 5,    ///< ARM MOV32 (A variant)
    ThumbMov32  = 7,    ///< Thumb MOV32 (T variant)
    Dir64       = 10,   ///< Full 64-bit value
};

// ============================================================================
// Relocation Structures
// ============================================================================

/// Base relocation block header
struct BaseRelocBlock {
    u32 page_rva;       ///< Page RVA (virtual address of block)
    u32 block_size;     ///< Size of block including header and entries
    
    /// Number of entries that follow this header
    [[nodiscard]] u32 entry_count() const noexcept {
        if (block_size < sizeof(BaseRelocBlock)) {
            return 0;
        }
        return (block_size - sizeof(BaseRelocBlock)) / sizeof(u16);
    }
};

static_assert(sizeof(BaseRelocBlock) == 8, "BaseRelocBlock must be 8 bytes");

/// Parsed relocation entry
struct RelocEntry {
    RelocType type;     ///< Relocation type (high 4 bits)
    u16 offset;         ///< Offset within page (low 12 bits)
};

/// Relocation statistics
struct RelocStats {
    u32 blocks_processed;
    u32 entries_applied;
    u32 entries_skipped;
    i64 delta;          ///< Image base delta applied
};

// ============================================================================
// Relocator Class
// ============================================================================

/// Processes PE base relocations
class Relocator {
public:
    /// Construct a relocator with guest memory
    explicit Relocator(GuestMemory& memory) noexcept;
    
    /// Apply base relocations for a PE loaded at a different address
    /// @param pe Parsed PE information
    /// @param pe_data Raw PE file data
    /// @param actual_base The actual address where the image was loaded
    /// @return Statistics on success, error on failure
    [[nodiscard]] Result<RelocStats, RelocError> apply(
        const ParsedPe& pe,
        std::span<const u8> pe_data,
        u64 actual_base
    ) noexcept;
    
    /// Check if relocations are needed
    /// @param pe Parsed PE information
    /// @param actual_base The actual address where the image was loaded
    /// @return true if delta != 0 and relocations exist
    [[nodiscard]] static bool needs_relocation(
        const ParsedPe& pe,
        u64 actual_base
    ) noexcept;
    
    /// Parse a single relocation entry from a 16-bit value
    [[nodiscard]] static RelocEntry parse_entry(u16 value) noexcept;
    
private:
    GuestMemory& m_memory;
    
    /// Process a single relocation block
    [[nodiscard]] Result<u32, RelocError> process_block(
        const BaseRelocBlock& block,
        std::span<const u16> entries,
        u64 base,
        i64 delta
    ) noexcept;
    
    /// Apply a single relocation entry
    [[nodiscard]] Result<void, RelocError> apply_entry(
        RelocEntry entry,
        u64 page_address,
        i64 delta
    ) noexcept;
};

// ============================================================================
// Free Functions
// ============================================================================

/// Apply relocations to a mapped PE image
/// @param memory Guest memory containing the mapped image
/// @param pe Parsed PE information
/// @param pe_data Raw PE file data
/// @param actual_base The actual address where the image was loaded
/// @return Statistics on success, error on failure
[[nodiscard]] Result<RelocStats, RelocError> apply_relocations(
    GuestMemory& memory,
    const ParsedPe& pe,
    std::span<const u8> pe_data,
    u64 actual_base
) noexcept;

} // namespace vitreo::pe
