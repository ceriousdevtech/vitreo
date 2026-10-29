// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CLI-002: reading the main image from disk or an optical image, and relocating it

#include "run_internal.hpp"
#include "capsule/vfs/image_mount_store.hpp"
#include "capsule/vfs/iso_filesystem.hpp"
#include "capsule/vfs/vfs.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/pe/loader/load_config.hpp"
#include "core/pe/mapper/image_placement.hpp"
#include "core/pe/mapper/section_mapper.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "core/pe/reloc/relocator.hpp"

#include <filesystem>
#include <fstream>
#include <span>
#include <system_error>

namespace vitreo::cli {

namespace fs = std::filesystem;

/// Read an entire file into a byte vector
RunResult read_file_bytes(const std::string& path, std::vector<u8>& out) {
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        return RunResult{1, 0, "File not found: " + path, false};
    }

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return RunResult{1, 0, "Failed to open file: " + path, false};
    }

    auto size = file.tellg();
    if (size <= 0) {
        return RunResult{1, 0, "File is empty or unreadable: " + path, false};
    }

    out.resize(static_cast<usize>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(out.data()), size);

    if (!file) {
        return RunResult{1, 0, "Failed to read file: " + path, false};
    }

    return RunResult{0, 0, {}, true};
}

RunResult read_image_file_bytes(const std::string& capsule_root,
                                const std::string& windows_path,
                                std::vector<u8>& out) {
    auto parsed = capsule::parse_windows_path(windows_path);
    capsule::ImageMountStore mounts(capsule_root);
    for (const auto& record : mounts.list()) {
        if (record.drive != parsed.drive_letter) continue;

        capsule::IsoFileSystem filesystem(record.image_path);
        if (!filesystem.valid()) {
            return RunResult{1, 0, "Unable to mount optical image: " +
                                   filesystem.error_message(), false};
        }
        auto info = filesystem.stat(parsed.relative_path);
        if (info.is_err() || has_attribute(info.value().attributes,
                                           capsule::FileAttributes::Directory)) {
            return RunResult{1, 0, "File not found in optical image: " + windows_path, false};
        }
        auto opened = filesystem.open(parsed.relative_path, capsule::AccessMode::Read,
                                      capsule::ShareMode::Read,
                                      capsule::CreationDisposition::OpenExisting);
        if (opened.is_err()) {
            return RunResult{1, 0, "Unable to open optical-image file: " + windows_path, false};
        }
        out.resize(static_cast<usize>(info.value().size));
        usize offset = 0;
        while (offset < out.size()) {
            auto read = filesystem.read(opened.value(),
                                        std::span<u8>(out).subspan(offset));
            if (read.is_err() || read.value() == 0) {
                (void)filesystem.close(opened.value());
                return RunResult{1, 0, "Unable to read optical-image file: " + windows_path, false};
            }
            offset += read.value();
        }
        (void)filesystem.close(opened.value());
        return RunResult{0, 0, {}, true};
    }
    return RunResult{1, 0, "Drive is not mounted: " + windows_path.substr(0, 2), false};
}

std::optional<RunResult> apply_main_image_relocations(
    const RunConfig& config, GuestMemory& memory, const pe::ParsedPe& pe,
    const std::vector<u8>& pe_data, const pe::MappedImage& image,
    const pe::ImagePlacement& placement) {
    // The Windows loader relocates before enforcing final section protections.
    // SectionMapper has already applied them from the section characteristics,
    // so a fixup landing in read-only .text or .rdata would fail. This only
    // matters when the image actually moved — at its preferred base every
    // delta is zero and nothing is written. PeLoader does the same for DLLs.
    if (placement.relocated) {
        if (image.header_size != 0) {
            auto pr = memory.protect_region(image.base_address,
                                            image.header_size,
                                            MemoryProtection::ReadWrite);
            if (!pr) {
                return RunResult{1, 0,
                    "Failed to unprotect PE headers for relocation", false};
            }
        }
        for (const auto& s : image.sections) {
            if (s.size == 0) continue;
            auto pr = memory.protect_region(s.guest_address, s.size,
                                            MemoryProtection::ReadWrite);
            if (!pr) {
                return RunResult{1, 0,
                    std::string("Failed to unprotect section '") + s.name +
                        "' for relocation",
                    false};
            }
        }
    }

    if (pe.has_relocations()) {
        auto reloc_result = pe::apply_relocations(
            memory, pe, std::span<const u8>(pe_data), image.base_address);

        if (reloc_result.is_err()) {
            // NoRelocations is not fatal — just means delta was 0 or no table
            if (reloc_result.error() != pe::RelocError::NoRelocations) {
                return RunResult{1, 0,
                    std::string("Relocation error: ") +
                        pe::reloc_error_message(reloc_result.error()),
                    false};
            }
        } else if (config.verbose) {
            const auto& stats = reloc_result.value();
            VITREO_TRACE( "[vitreo] Relocations: %u blocks, "
                         "%u applied, %u skipped, delta=%lld\n",
                         stats.blocks_processed,
                         stats.entries_applied,
                         stats.entries_skipped,
                         static_cast<long long>(stats.delta));
        }
    }

    // Match the Windows image loader: initialize the load-config /GS cookie
    // before the executable's TLS callbacks or entry point can run.
    if (!pe::initialize_load_config_security_cookie(
            memory, pe, image.base_address)) {
        return RunResult{1, 0,
            "Invalid PE load-config security cookie", false};
    }

    // Windows updates the in-memory OptionalHeader.ImageBase to the actual load
    // address; packers and CRT startup read it back to compute their own delta.
    // PeLoader does this for every DLL — the main image only ever moves under
    // identity mapping, so it is done here too rather than left inconsistent.
    if (placement.relocated) {
        if (auto lfanew = memory.read_u32(image.base_address + 0x3Cu);
            lfanew.is_ok() && lfanew.value() != 0) {
            const GuestAddr opt_base =
                image.base_address + lfanew.value() + 4u + sizeof(pe::CoffHeader);
            if (pe.is_64bit) {
                (void)memory.write_u64(opt_base + 24u, image.base_address);
            } else {
                (void)memory.write_u32(
                    opt_base + 28u, static_cast<u32>(image.base_address));
            }
        }

        // Restore the protections lifted for relocation.
        for (const auto& s : image.sections) {
            if (s.size == 0) continue;
            (void)memory.protect_region(s.guest_address, s.size, s.protection);
        }
        if (image.header_size != 0) {
            (void)memory.protect_region(image.base_address, image.header_size,
                                        MemoryProtection::Read);
        }
    }

    return std::nullopt;
}

} // namespace vitreo::cli
