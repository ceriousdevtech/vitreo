// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// PE base relocation processor

#include "relocator.hpp"
#include <cstring>
#include <cstdio>

namespace vitreo::pe {

// ============================================================================
// Error Messages
// ============================================================================

const char* reloc_error_message(RelocError err) noexcept {
    switch (err) {
        case RelocError::Success:
            return "Success";
        case RelocError::NoRelocations:
            return "No relocations present";
        case RelocError::InvalidRelocBlock:
            return "Invalid relocation block";
        case RelocError::InvalidRelocEntry:
            return "Invalid relocation entry";
        case RelocError::MemoryReadFailed:
            return "Failed to read memory";
        case RelocError::MemoryWriteFailed:
            return "Failed to write memory";
        case RelocError::UnsupportedRelocType:
            return "Unsupported relocation type";
        case RelocError::AddressOverflow:
            return "Address overflow during relocation";
    }
    return "Unknown error";
}

// ============================================================================
// Relocator Construction
// ============================================================================

Relocator::Relocator(GuestMemory& memory) noexcept
    : m_memory(memory) {}

// ============================================================================
// Static Helpers
// ============================================================================

bool Relocator::needs_relocation(const ParsedPe& pe, u64 actual_base) noexcept {
    // No relocations needed if loaded at preferred base
    if (actual_base == pe.image_base) {
        return false;
    }
    
    // Check if relocations exist
    return pe.has_relocations();
}

RelocEntry Relocator::parse_entry(u16 value) noexcept {
    RelocEntry entry;
    entry.type = static_cast<RelocType>((value >> 12) & 0x0F);
    entry.offset = value & 0x0FFF;
    return entry;
}

// ============================================================================
// Main Apply Function
// ============================================================================

Result<RelocStats, RelocError> Relocator::apply(
    const ParsedPe& pe,
    std::span<const u8> pe_data,
    u64 actual_base
) noexcept {
    RelocStats stats{};
    stats.delta = static_cast<i64>(actual_base) - static_cast<i64>(pe.image_base);
    
    // If no delta, nothing to do
    if (stats.delta == 0) {
        return Result<RelocStats, RelocError>::ok(stats);
    }
    
    // Check for relocations
    if (!pe.has_relocations()) {
        return Result<RelocStats, RelocError>::err(RelocError::NoRelocations);
    }
    
    const auto& reloc_dir = pe.data_directories[DIR_BASERELOC];
    
    // Convert RVA to file offset
    auto offset_result = pe.rva_to_file_offset(reloc_dir.VirtualAddress);
    if (!offset_result) {
        return Result<RelocStats, RelocError>::err(RelocError::InvalidRelocBlock);
    }
    
    u32 reloc_offset = offset_result.value();
    u32 reloc_size = reloc_dir.Size;
    u32 current_offset = 0;
    
    // Process each relocation block
    while (current_offset < reloc_size) {
        // Check we have enough data for block header
        if (reloc_offset + current_offset + sizeof(BaseRelocBlock) > pe_data.size()) {
            break;
        }
        
        // Read block header
        BaseRelocBlock block;
        std::memcpy(&block, pe_data.data() + reloc_offset + current_offset, sizeof(block));
        
        // Validate block
        if (block.block_size < sizeof(BaseRelocBlock)) {
            break;  // End of relocations
        }
        
        if (block.block_size > reloc_size - current_offset) {
            return Result<RelocStats, RelocError>::err(RelocError::InvalidRelocBlock);
        }
        
        // Get entries that follow the block header
        u32 entry_count = block.entry_count();
        u32 entries_offset = reloc_offset + current_offset + sizeof(BaseRelocBlock);
        
        if (entries_offset + entry_count * sizeof(u16) > pe_data.size()) {
            return Result<RelocStats, RelocError>::err(RelocError::InvalidRelocBlock);
        }
        
        // Create span of entries
        std::span<const u16> entries(
            reinterpret_cast<const u16*>(pe_data.data() + entries_offset),
            entry_count
        );
        
        // Process this block
        auto block_result = process_block(block, entries, actual_base, stats.delta);
        if (!block_result) {
            return Result<RelocStats, RelocError>::err(block_result.error());
        }
        
        stats.entries_applied += block_result.value();
        stats.blocks_processed++;
        
        // Move to next block
        current_offset += block.block_size;
    }
    
    return Result<RelocStats, RelocError>::ok(stats);
}

// ============================================================================
// Block Processing
// ============================================================================

Result<u32, RelocError> Relocator::process_block(
    const BaseRelocBlock& block,
    std::span<const u16> entries,
    u64 base,
    i64 delta
) noexcept {
    u64 page_address = base + block.page_rva;
    u32 applied = 0;
    
    for (u16 entry_value : entries) {
        RelocEntry entry = parse_entry(entry_value);
        
        // Skip ABSOLUTE (padding) entries
        if (entry.type == RelocType::Absolute) {
            continue;
        }
        
        auto result = apply_entry(entry, page_address, delta);
        if (!result) {
            return Result<u32, RelocError>::err(result.error());
        }
        
        applied++;
    }
    
    return Result<u32, RelocError>::ok(applied);
}

// ============================================================================
// Entry Application
// ============================================================================

Result<void, RelocError> Relocator::apply_entry(
    RelocEntry entry,
    u64 page_address,
    i64 delta
) noexcept {
    u64 target_address = page_address + entry.offset;
    
    switch (entry.type) {
        case RelocType::Absolute:
            // No-op (padding)
            return Result<void, RelocError>::ok();
            
        case RelocType::HighLow: {
            // 32-bit relocation
            auto read_result = m_memory.read_u32(target_address);
            if (!read_result) {
                return Result<void, RelocError>::err(RelocError::MemoryReadFailed);
            }
            
            u32 original = read_result.value();
            u32 relocated = static_cast<u32>(static_cast<i64>(original) + delta);
            
            // Debug: log relocations in code section to verify they're applied
            bool is_interesting = (original >= 0x6fc10000 && original < 0x6fc70000) ||
                                  (target_address >= 0x18001800 && target_address < 0x18001850);
            if (is_interesting) {
                VITREO_TRACE(
                             "[reloc] HIGHLOW @ 0x%llx: 0x%08x -> 0x%08x\n",
                             static_cast<unsigned long long>(target_address),
                             original, relocated);
            }
            
            auto write_result = m_memory.write_u32(target_address, relocated);
            if (!write_result) {
                return Result<void, RelocError>::err(RelocError::MemoryWriteFailed);
            }
            
            return Result<void, RelocError>::ok();
        }
        
        case RelocType::Dir64: {
            // 64-bit relocation
            auto read_result = m_memory.read_u64(target_address);
            if (!read_result) {
                return Result<void, RelocError>::err(RelocError::MemoryReadFailed);
            }
            
            u64 original = read_result.value();
            
            // Check for overflow
            if (delta > 0 && original > UINT64_MAX - static_cast<u64>(delta)) {
                return Result<void, RelocError>::err(RelocError::AddressOverflow);
            }
            if (delta < 0 && original < static_cast<u64>(-delta)) {
                return Result<void, RelocError>::err(RelocError::AddressOverflow);
            }
            
            u64 relocated = static_cast<u64>(static_cast<i64>(original) + delta);

            auto write_result = m_memory.write_u64(target_address, relocated);
            if (!write_result) {
                return Result<void, RelocError>::err(RelocError::MemoryWriteFailed);
            }
            
            return Result<void, RelocError>::ok();
        }
        
        case RelocType::High: {
            // High 16 bits of 32-bit value
            auto read_result = m_memory.read_u16(target_address);
            if (!read_result) {
                return Result<void, RelocError>::err(RelocError::MemoryReadFailed);
            }
            
            u16 original = read_result.value();
            u32 full_value = static_cast<u32>(original) << 16;
            u32 relocated = static_cast<u32>(static_cast<i64>(full_value) + delta);
            u16 new_high = static_cast<u16>(relocated >> 16);
            
            auto write_result = m_memory.write_u16(target_address, new_high);
            if (!write_result) {
                return Result<void, RelocError>::err(RelocError::MemoryWriteFailed);
            }
            
            return Result<void, RelocError>::ok();
        }
        
        case RelocType::Low: {
            // Low 16 bits of 32-bit value
            auto read_result = m_memory.read_u16(target_address);
            if (!read_result) {
                return Result<void, RelocError>::err(RelocError::MemoryReadFailed);
            }
            
            u16 original = read_result.value();
            u32 relocated = static_cast<u32>(static_cast<i64>(original) + delta);
            u16 new_low = static_cast<u16>(relocated & 0xFFFF);
            
            auto write_result = m_memory.write_u16(target_address, new_low);
            if (!write_result) {
                return Result<void, RelocError>::err(RelocError::MemoryWriteFailed);
            }
            
            return Result<void, RelocError>::ok();
        }
        
        default:
            // Unsupported relocation type
            return Result<void, RelocError>::err(RelocError::UnsupportedRelocType);
    }
}

// ============================================================================
// Free Function
// ============================================================================

Result<RelocStats, RelocError> apply_relocations(
    GuestMemory& memory,
    const ParsedPe& pe,
    std::span<const u8> pe_data,
    u64 actual_base
) noexcept {
    Relocator relocator(memory);
    return relocator.apply(pe, pe_data, actual_base);
}

} // namespace vitreo::pe
