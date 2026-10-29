// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
/// @file cor_image.hpp
/// @brief What the Windows (8+) loader does with images that carry a COR20
///        header (IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR).
///
/// Learn (_CorValidateImage remarks) plus the oracle
/// (tests/oracle_data/cor_loader_probe{32,64}.expected):
///  - An IL-only (COMIMAGE_FLAGS_ILONLY) EXE does not run its own entry
///    point: the loader starts the process at mscoree!_CorExeMain, and the
///    image's loader entry (LDR_DATA_TABLE_ENTRY.EntryPoint) is NULL. A PE32+
///    IL-only exe has AddressOfEntryPoint 0.
///  - An IL-only DLL is data to the loader: entry NULL, mscoree is not
///    loaded, nothing in it runs.
///  - An IL-only image is not relocated and its in-memory
///    OptionalHeader.ImageBase keeps the file's value.
///  - A mixed-mode (COR header, not ILONLY) image is a native image: imports
///    bound, its own entry point is the loader entry (a C++/CLI image's entry
///    is the import jump to mscoree!_CorExeMain / _CorDllMain).
///  - A PE32 IL-only image that does not require 32-bit (no
///    COMIMAGE_FLAGS_32BITREQUIRED) runs as a 64-bit process on 64-bit
///    Windows, and in a 64-bit process the mapped image is rewritten from
///    PE32 to PE32+ (rewrite_il_only_pe32_as_pe32plus). With
///    32BITREQUIRED (/platform:x86, anycpu32bitpreferred) it stays 32-bit.

#pragma once

#include <vitreo/common.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vitreo::pe {

/// winnt.h ReplacesCorHdrNumericDefines.
inline constexpr u32 COMIMAGE_FLAGS_ILONLY = 0x00000001;
inline constexpr u32 COMIMAGE_FLAGS_32BITREQUIRED = 0x00000002;
inline constexpr u32 COMIMAGE_FLAGS_32BITPREFERRED = 0x00020000;

struct CorImageInfo {
    bool pe32plus{false};   ///< OptionalHeader.Magic == 0x20b
    u32 flags{0};           ///< IMAGE_COR20_HEADER.Flags
    std::string metadata_version;  ///< the metadata root's version string
};

/// The COR20 facts of a PE file, or nullopt when it has no COR header
/// (or the header does not lie inside the file).
[[nodiscard]] std::optional<CorImageInfo> read_cor_image(std::span<const u8> file);

[[nodiscard]] inline bool is_il_only(const std::optional<CorImageInfo>& cor) {
    return cor && (cor->flags & COMIMAGE_FLAGS_ILONLY) != 0;
}

/// A PE32 IL-only image without 32BITREQUIRED: a 64-bit process on 64-bit
/// Windows (and loadable into one).
[[nodiscard]] inline bool il_only_pe32_runs_64bit(const std::optional<CorImageInfo>& cor) {
    return is_il_only(cor) && !cor->pe32plus && (cor->flags & COMIMAGE_FLAGS_32BITREQUIRED) == 0;
}

/// The rewrite the 64-bit loader applies to such an image (oracle field by
/// field): Magic 0x10b -> 0x20b; SizeOfOptionalHeader += 16; ImageBase
/// (formerly BaseOfData + ImageBase) becomes the 8-byte zero-extended
/// ImageBase; the four stack/heap sizes become 8-byte zero-extended
/// values; LoaderFlags, NumberOfRvaAndSizes, the data directories and the
/// section table move up 16 bytes unchanged. Machine, AddressOfEntryPoint,
/// SizeOfHeaders and every other field keep their values. Returns false,
/// leaving @p file unchanged, when the image is not a PE32 IL-only image or
/// the headers have no room for the 16 extra bytes.
[[nodiscard]] bool rewrite_il_only_pe32_as_pe32plus(std::vector<u8>& file);

}  // namespace vitreo::pe
