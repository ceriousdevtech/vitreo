// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file cabinet.hpp
/// @brief Microsoft Cabinet (MSCF) container reader.
///
/// Parses the container described by [MS-CAB] and hands back folders, file
/// entries, and decompressed bytes. This is a pure data-format reader: bytes
/// in, bytes out, no guest memory, no VFS, no Win32.
///
/// The layout landmine this reader exists to get right: **the CFHEADER is not
/// a fixed 36 bytes**. When cfhdrRESERVE_PRESENT is set, three reserve-size
/// fields and a per-cabinet reserve area of up to 60000 bytes follow it, and
/// two more reserve sizes apply to *every* CFFOLDER and *every* CFDATA in the
/// file. Real installer cabinets (a 4096-byte header reserve is common — it is
/// where a digital signature goes) put nothing readable at offset 36, so a
/// parser that assumes the fixed size reads folder records out of the middle
/// of a signature blob and reports plausible-looking garbage.

#include <vitreo/common.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::compression::cabinet {

/// CFHEADER.flags
inline constexpr u16 kFlagPrevCabinet = 0x0001;
inline constexpr u16 kFlagNextCabinet = 0x0002;
inline constexpr u16 kFlagReservePresent = 0x0004;

/// CFFILE.iFolder sentinels for files that straddle cabinet boundaries.
inline constexpr u16 kFolderContinuedFromPrev = 0xFFFD;
inline constexpr u16 kFolderContinuedToNext = 0xFFFE;
inline constexpr u16 kFolderContinuedPrevAndNext = 0xFFFF;

/// CFFILE.attribs. _A_NAME_IS_UTF decides how szName is to be interpreted;
/// without it the name is codepage bytes, not UTF-8, and must not be handed to
/// a UTF-8 consumer unconverted.
inline constexpr u16 kAttrReadOnly = 0x0001;
inline constexpr u16 kAttrHidden = 0x0002;
inline constexpr u16 kAttrSystem = 0x0004;
inline constexpr u16 kAttrArchive = 0x0020;
inline constexpr u16 kAttrExec = 0x0040;
inline constexpr u16 kAttrNameIsUtf = 0x0080;

enum class CompressionType : u8 {
    None = 0,
    MsZip = 1,
    Quantum = 2,
    Lzx = 3,
};

struct Header {
    u32 cabinet_size = 0;      ///< cbCabinet
    u32 files_offset = 0;      ///< coffFiles
    u8 version_major = 0;
    u8 version_minor = 0;
    u16 folder_count = 0;
    u16 file_count = 0;
    u16 flags = 0;
    u16 set_id = 0;
    u16 cabinet_index = 0;

    /// Reserve sizes. Zero unless kFlagReservePresent is set — but note that
    /// the flag being set does not imply all three are non-zero.
    u16 header_reserve_size = 0;  ///< cbCFHeader
    u8 folder_reserve_size = 0;   ///< cbCFFolder
    u8 data_reserve_size = 0;     ///< cbCFData

    std::string previous_cabinet;
    std::string previous_disk;
    std::string next_cabinet;
    std::string next_disk;
};

struct Folder {
    u32 data_offset = 0;        ///< coffCabStart: file offset of first CFDATA
    u16 data_block_count = 0;   ///< cCFData
    u16 raw_compression = 0;    ///< typeCompress, unmasked
    CompressionType compression = CompressionType::None;
    /// log2 of the LZX window, from the high byte of typeCompress. Only
    /// meaningful for CompressionType::Lzx.
    u8 lzx_window_bits = 0;
};

struct FileEntry {
    std::string name;
    u32 size = 0;           ///< cbFile
    u32 folder_offset = 0;  ///< uoffFolderStart
    u16 folder_index = 0;   ///< iFolder, sentinels preserved verbatim
    u16 date = 0;
    u16 time = 0;
    u16 attributes = 0;

    [[nodiscard]] bool name_is_utf8() const noexcept {
        return (attributes & kAttrNameIsUtf) != 0;
    }
    /// True for the iFolder sentinels, i.e. this entry's bytes are not wholly
    /// inside this cabinet's folder list.
    [[nodiscard]] bool spans_cabinets() const noexcept {
        return folder_index >= kFolderContinuedFromPrev;
    }
};

/// One CFDATA record, resolved to absolute positions in the image.
struct DataBlock {
    u32 checksum = 0;
    u16 compressed_size = 0;
    u16 uncompressed_size = 0;
    usize payload_offset = 0;  ///< absolute file offset of ab[]
};

class Cabinet {
public:
    /// Parse the container structures. The returned Cabinet keeps a view of
    /// @p image; the caller owns the bytes and must outlive it.
    [[nodiscard]] static Result<Cabinet, Error> parse(std::span<const u8> image);

    [[nodiscard]] const Header& header() const noexcept { return header_; }
    [[nodiscard]] const std::vector<Folder>& folders() const noexcept { return folders_; }
    [[nodiscard]] const std::vector<FileEntry>& files() const noexcept { return files_; }

    /// Walk a folder's CFDATA chain. Cheap; does not decompress.
    [[nodiscard]] Result<std::vector<DataBlock>, Error> enumerate_blocks(
        usize folder_index) const;

    /// Decompress a whole folder. A folder is the unit of compression: its
    /// blocks share one LZX state, so there is no cheaper way to reach a file
    /// than to decode everything up to it.
    [[nodiscard]] Result<std::vector<u8>, Error> extract_folder(usize folder_index) const;

    /// Extract one file. Decompresses that file's whole folder, so extracting
    /// many files one at a time is quadratic — use extract_folder() and slice
    /// when pulling more than a couple of files.
    [[nodiscard]] Result<std::vector<u8>, Error> extract_file(const FileEntry& file) const;
    [[nodiscard]] Result<std::vector<u8>, Error> extract_file(usize file_index) const;
    [[nodiscard]] Result<std::vector<u8>, Error> extract_file_by_name(
        std::string_view name) const;

    /// Slice one file's bytes out of already-decompressed folder data.
    [[nodiscard]] static Result<std::span<const u8>, Error> slice_file(
        const FileEntry& file, std::span<const u8> folder_data);

    [[nodiscard]] const FileEntry* find_file(std::string_view name) const;

private:
    std::span<const u8> image_;
    Header header_;
    std::vector<Folder> folders_;
    std::vector<FileEntry> files_;
};

/// The checksum [MS-CAB] defines for CFDATA: a 32-bit XOR fold, with the
/// trailing 1..3 bytes folded in most-significant-byte first. Exposed because
/// it is the only integrity check the format offers, and cabinets are free to
/// store 0 to mean "not computed".
[[nodiscard]] u32 compute_checksum(std::span<const u8> data, u32 seed) noexcept;

} // namespace vitreo::compression::cabinet
