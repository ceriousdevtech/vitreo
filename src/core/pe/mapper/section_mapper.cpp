// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// PE section mapper implementation

#include "section_mapper.hpp"
#include "image_layout_file.hpp"
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <unistd.h>
#include <vector>

namespace vitreo::pe {

namespace {

/// Smaller images keep the anonymous copy: a cache file per tiny DLL costs
/// more than the few pages it would share.
constexpr usize kMinFileBackedImage = usize{1} << 20;

[[nodiscard]] constexpr u32 align_up_u32(u32 value, u32 alignment) noexcept {
    if (alignment == 0) {
        return value;
    }
    return (value + alignment - 1u) & ~(alignment - 1u);
}

/// Windows maps each section to align_up(max(VirtualSize, SizeOfRawData),
/// SectionAlignment) when VirtualSize is non-zero; a zero VirtualSize falls
/// back to SizeOfRawData. Packers (Petite, UPX, …) often set SizeOfRawData >
/// VirtualSize for the stub section — truncating the copy leaves zeros that
/// make their CRC fail with "Corrupt Data!".
[[nodiscard]] u32 section_map_size(const ParsedSection& section,
                                   u32 section_alignment) noexcept {
    u32 virtual_size = section.virtual_size;
    if (virtual_size == 0) {
        virtual_size = section.raw_size;
    }
    const u32 span = std::max(virtual_size, section.raw_size);
    u32 aligned = align_up_u32(span, section_alignment);
    if (aligned < 0x1000u) {
        aligned = 0x1000u;
    }
    return aligned;
}

/// Span of the PE header pages: page-aligned SizeOfHeaders, clipped to the
/// lowest section RVA so we never overlap a mapped section.
[[nodiscard]] u32 pe_header_span(const ParsedPe& pe) noexcept {
    u32 span = align_up_u32(pe.size_of_headers, 0x1000u);
    if (span == 0) {
        span = 0x1000u;
    }
    u32 min_section_va = 0xFFFFFFFFu;
    for (const auto& section : pe.sections) {
        if (section.virtual_address != 0 &&
            section.virtual_address < min_section_va) {
            min_section_va = section.virtual_address;
        }
    }
    if (min_section_va != 0xFFFFFFFFu && min_section_va < span) {
        span = min_section_va;
    }
    return span;
}

}  // namespace

// ============================================================================
// Error Messages
// ============================================================================

const char* map_error_message(MapError err) noexcept {
    switch (err) {
        case MapError::Success:
            return "Success";
        case MapError::InvalidPe:
            return "Invalid PE file";
        case MapError::InvalidSection:
            return "Invalid section";
        case MapError::MemoryMapFailed:
            return "Failed to map memory region";
        case MapError::ProtectionFailed:
            return "Failed to set memory protection";
        case MapError::SectionOverlap:
            return "Section overlap detected";
        case MapError::ImageTooLarge:
            return "Image too large to map";
        case MapError::AddressOverflow:
            return "Address overflow";
    }
    return "Unknown error";
}

// ============================================================================
// Protection Conversion
// ============================================================================

MemoryProtection SectionMapper::section_to_protection(u32 characteristics) noexcept {
    // Start with no permissions
    u32 prot = 0;
    
    // Check read permission (IMAGE_SCN_MEM_READ)
    if (characteristics & SCN_MEM_READ) {
        prot |= static_cast<u32>(MemoryProtection::Read);
    }
    
    // Check write permission (IMAGE_SCN_MEM_WRITE)
    if (characteristics & SCN_MEM_WRITE) {
        prot |= static_cast<u32>(MemoryProtection::Write);
    }
    
    // Check execute permission (IMAGE_SCN_MEM_EXECUTE)
    if (characteristics & SCN_MEM_EXECUTE) {
        prot |= static_cast<u32>(MemoryProtection::Execute);
    }
    
    // If no permissions set, default to read
    if (prot == 0) {
        prot = static_cast<u32>(MemoryProtection::Read);
    }

    // Honor PE section characteristics for VirtualQuery / access checks.
    // Windows maps non-writable image pages as PAGE_READONLY (with
    // copy-on-write underneath). The guest asserts
    // VirtualQuery.Protect == PAGE_READONLY on a read-only section such as
    // "prot". Mapping read-only initialized data as read-write makes that
    // check fire (UD2). True copy-on-write on store for in-place
    // .rdata/resource patches remains a separate gap. Callers that
    // VirtualProtect first still work with characteristics-faithful
    // protections.

    return static_cast<MemoryProtection>(prot);
}

/// Win32 PAGE_* Windows reports for an unwritten page of a PE section.
/// Writable sections are PAGE_*WRITECOPY until privately written
/// (virtual_memory_contract_probe.expected img q / img .data after write).
u32 section_to_win32_protection(u32 characteristics) noexcept {
    const bool write = (characteristics & SCN_MEM_WRITE) != 0;
    const bool exec = (characteristics & SCN_MEM_EXECUTE) != 0;
    const bool read = (characteristics & SCN_MEM_READ) != 0;
    if (write && exec) return 0x80; // PAGE_EXECUTE_WRITECOPY
    if (write) return 0x08;         // PAGE_WRITECOPY
    if (exec && read) return 0x20;  // PAGE_EXECUTE_READ
    if (exec) return 0x10;          // PAGE_EXECUTE
    if (read) return 0x02;          // PAGE_READONLY
    return 0x01;                    // PAGE_NOACCESS
}

// ============================================================================
// Main Mapping Function
// ============================================================================

Result<MappedImage, MapError> SectionMapper::map(
    const ParsedPe& pe,
    std::span<const u8> pe_data,
    u64 load_address
) noexcept {
    MappedImage image{};
    
    // Validate PE
    if (pe.sections.empty() && pe.size_of_image == 0) {
        return Result<MappedImage, MapError>::err(MapError::InvalidPe);
    }
    
    // Determine base address
    u64 base = load_address;
    if (base == 0) {
        base = pe.image_base;
    }
    image.base_address = base;
    image.image_size = pe.size_of_image;
    
    // Calculate entry point
    if (pe.entry_point_rva != 0) {
        image.entry_point = base + pe.entry_point_rva;
    } else {
        image.entry_point = 0;
    }

    // Windows maps a PE as one contiguous SizeOfImage MEM_IMAGE reservation.
    // Mapping headers and each section as separate GuestMemory regions breaks
    // dword/byte accesses that straddle section VAs — a common pattern in
    // packer stubs (Petite, UPX, …) during in-place expand/decompress.
    if (pe.size_of_image == 0) {
        return Result<MappedImage, MapError>::err(MapError::InvalidPe);
    }
    const u32 image_aligned =
        align_up_u32(pe.size_of_image, 0x1000u);
    if (image_aligned < pe.size_of_image) {
        return Result<MappedImage, MapError>::err(MapError::AddressOverflow);
    }
    image.image_size = image_aligned;

    // The image is the PE headers followed by each section's raw data at its
    // RVA, in that order, over zeros.
    std::vector<ImageLayoutCopy> copies;
    copies.reserve(pe.sections.size() + 1);
    const usize header_copy =
        std::min<usize>(pe.size_of_headers, pe_data.size());
    if (header_copy != 0) {
        copies.push_back({0, 0, header_copy});
    }

    // Record per-section protection metadata for VirtualProtect / VirtualQuery.
    image.sections.reserve(pe.sections.size());
    for (const auto& section : pe.sections) {
        MappedSection mapped{};
        std::memcpy(mapped.name, section.name, 8);
        mapped.name[8] = '\0';

        if (section.virtual_address >= image_aligned) {
            return Result<MappedImage, MapError>::err(MapError::InvalidSection);
        }

        const u32 aligned_size =
            section_map_size(section, pe.section_alignment);
        if (section.virtual_address + aligned_size > image_aligned &&
            section.virtual_address < image_aligned) {
            // Clamp to SizeOfImage — Windows does not map past the image.
            mapped.size = image_aligned - section.virtual_address;
        } else {
            mapped.size = aligned_size;
        }
        mapped.guest_address = base + section.virtual_address;
        mapped.protection = section_to_protection(section.characteristics);
        mapped.win32_protection =
            section_to_win32_protection(section.characteristics);

        if (section.raw_size > 0 && section.raw_offset > 0 &&
            section.raw_offset < pe_data.size()) {
            const usize file_avail = pe_data.size() - section.raw_offset;
            const usize copy_size = std::min<usize>(
                std::min<usize>(section.raw_size, mapped.size),
                file_avail);
            if (copy_size != 0) {
                copies.push_back({section.virtual_address,
                                  static_cast<usize>(section.raw_offset),
                                  copy_size});
            }
        }

        image.sections.push_back(mapped);
    }

    auto map_result = m_memory.map_region(
        base,
        image_aligned,
        MemoryProtection::ReadWrite
    );
    if (!map_result) {
        MapError me = (map_result.error() == Error::AlreadyExists)
                          ? MapError::SectionOverlap
                          : MapError::MemoryMapFailed;
        return Result<MappedImage, MapError>::err(me);
    }

    // Windows maps SEC_IMAGE pages from the file: an unwritten page is clean
    // and shared by every process mapping the image. Back every whole host
    // page of the image with a laid-out copy of the file so only pages that
    // are relocated, bound, or written become private. Edge host pages that
    // the image shares with a neighbour stay anonymous and are copied.
    GuestAddr backed_lo = 0;
    GuestAddr backed_hi = 0;
    const usize hp = usize{1} << kLinearHostPageShift;
    if (image_aligned >= kMinFileBackedImage &&
        (static_cast<usize>(base) & (hp - 1)) == 0 &&
        m_memory.linear_base() != nullptr && !m_memory.is_identity()) {
        const usize span = static_cast<usize>(image_aligned) & ~(hp - 1);
        const int fd = open_image_layout_file(pe_data, image_aligned, copies);
        if (fd >= 0) {
            if (m_memory.back_region_with_file(base, span, fd, 0)) {
                backed_lo = base;
                backed_hi = base + span;
            }
            ::close(fd);
        }
    }

    for (const auto& c : copies) {
        const GuestAddr lo = base + c.rva;
        const GuestAddr hi = lo + c.size;
        const u8* src = pe_data.data() + c.file_offset;
        // Only the parts outside the file-backed span still need bytes.
        const GuestAddr pieces[2][2] = {
            {lo, std::min(hi, std::max(lo, backed_lo))},
            {std::max(lo, backed_hi), hi},
        };
        for (const auto& [from, to] : pieces) {
            if (from >= to) continue;
            if (!m_memory.write_bytes(from, src + (from - lo),
                                      static_cast<usize>(to - from))) {
                (void)m_memory.unmap_region(base, image_aligned);
                return Result<MappedImage, MapError>::err(
                    MapError::MemoryMapFailed);
            }
        }
    }

    // Apply final per-section protections as runs inside the single region.
    for (const auto& mapped : image.sections) {
        if (mapped.size == 0) {
            continue;
        }
        (void)m_memory.protect_region(
            mapped.guest_address, mapped.size, mapped.protection);
    }

    // PE headers are PAGE_READONLY on Windows. Packers (Petite) store through
    // ImageBase to raise ACCESS_VIOLATION and enter SEH unpack.
    image.header_size = pe_header_span(pe);
    if (image.header_size > 0 && image.header_size <= image_aligned) {
        (void)m_memory.protect_region(
            base, image.header_size, MemoryProtection::Read);
    } else {
        image.header_size = 0;
    }
    
    return Result<MappedImage, MapError>::ok(std::move(image));
}

// ============================================================================
// Unmapping
// ============================================================================

Result<void, MapError> SectionMapper::unmap(const MappedImage& image) noexcept {
    // One contiguous image mapping — unmap once at the image base.
    if (image.image_size != 0) {
        (void)m_memory.unmap_region(image.base_address, image.image_size);
    }
    return Result<void, MapError>::ok();
}

// ============================================================================
// Convenience Function
// ============================================================================

Result<MappedImage, MapError> map_pe(
    GuestMemory& memory,
    const ParsedPe& pe,
    std::span<const u8> pe_data,
    u64 load_address
) noexcept {
    SectionMapper mapper(memory);
    return mapper.map(pe, pe_data, load_address);
}

} // namespace vitreo::pe
