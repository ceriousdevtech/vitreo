// SPDX-License-Identifier: LGPL-2.1-only
/// @file cabinet.cpp
/// @brief Microsoft Cabinet (MSCF) container reader implementation.
///
/// Written against Microsoft's published [MS-CAB] structure definitions. Field
/// order, sizes, and the reserve-area rules come from that document; no other
/// cabinet reader's source was consulted.

#include "core/compression/cabinet.hpp"

#include "core/compression/lzx.hpp"
#include "core/compression/mszip.hpp"

#include <cstring>

namespace vitreo::compression::cabinet {

namespace {

/// Fixed part of the CFHEADER, up to and including iCabinet. Everything after
/// this point is conditional on flags, which is exactly the trap documented in
/// cabinet.hpp: this is the *minimum* header size, never the actual one.
inline constexpr usize kFixedHeaderSize = 36;

/// CFFOLDER without its per-folder reserve.
inline constexpr usize kFolderRecordSize = 8;

/// CFFILE without its NUL-terminated name.
inline constexpr usize kFileRecordSize = 16;

/// CFDATA without its per-block reserve and payload.
inline constexpr usize kDataRecordSize = 8;

/// [MS-CAB] caps the per-cabinet reserve at 60000 bytes. A larger value is a
/// corrupt or hostile header, and accepting it would let a bad cabinet point
/// the folder cursor anywhere in the image.
inline constexpr u32 kMaxHeaderReserve = 60000;

/// tcompMASK_TYPE / tcompMASK_LZX_WINDOW from the folder's typeCompress field.
inline constexpr u16 kCompressTypeMask = 0x000F;
inline constexpr u16 kCompressLzxWindowMask = 0x1F00;
inline constexpr u32 kCompressLzxWindowShift = 8;

/// A CFDATA never decodes to more than one 32 KB LZX frame.
inline constexpr u32 kMaxBlockUncompressed = 32768;

/// Bounds-checked little-endian readers. Every structure field in a cabinet is
/// little endian regardless of host byte order, so these never widen a host
/// integer directly over the image.
class Cursor {
public:
    explicit Cursor(std::span<const u8> image, usize offset = 0) noexcept
        : image_(image), offset_(offset) {}

    [[nodiscard]] bool has(usize count) const noexcept {
        return offset_ <= image_.size() && (image_.size() - offset_) >= count;
    }
    [[nodiscard]] usize offset() const noexcept { return offset_; }
    void seek(usize offset) noexcept { offset_ = offset; }

    [[nodiscard]] u8 u8_() noexcept { return image_[offset_++]; }
    [[nodiscard]] u16 u16_() noexcept {
        const u16 v = static_cast<u16>(image_[offset_]) |
                      static_cast<u16>(static_cast<u16>(image_[offset_ + 1]) << 8);
        offset_ += 2;
        return v;
    }
    [[nodiscard]] u32 u32_() noexcept {
        const u32 v = static_cast<u32>(image_[offset_]) |
                      (static_cast<u32>(image_[offset_ + 1]) << 8) |
                      (static_cast<u32>(image_[offset_ + 2]) << 16) |
                      (static_cast<u32>(image_[offset_ + 3]) << 24);
        offset_ += 4;
        return v;
    }

    /// NUL-terminated szName / szDisk string. Returns false if the terminator
    /// is not inside the image — a cabinet that runs off the end mid-name is
    /// truncated, and the partial name must not be handed back as a success.
    [[nodiscard]] bool string_(std::string& out) {
        const usize start = offset_;
        while (offset_ < image_.size() && image_[offset_] != 0) ++offset_;
        if (offset_ >= image_.size()) return false;
        out.assign(reinterpret_cast<const char*>(image_.data() + start), offset_ - start);
        ++offset_;  // consume the NUL
        return true;
    }

private:
    std::span<const u8> image_;
    usize offset_ = 0;
};

}  // namespace

// =============================================================================
// Checksum
// =============================================================================

u32 compute_checksum(std::span<const u8> data, u32 seed) noexcept {
    u32 csum = seed;
    usize i = 0;
    const usize whole = data.size() & ~usize{3};
    for (; i < whole; i += 4) {
        csum ^= static_cast<u32>(data[i]) | (static_cast<u32>(data[i + 1]) << 8) |
                (static_cast<u32>(data[i + 2]) << 16) | (static_cast<u32>(data[i + 3]) << 24);
    }
    // The tail is folded most-significant-byte first, which is *not* the same
    // as zero-padding the final little-endian word: with three bytes left the
    // first goes to bits 16..23, not 0..7.
    u32 tail = 0;
    switch (data.size() - i) {
        case 3:
            tail |= static_cast<u32>(data[i]) << 16;
            tail |= static_cast<u32>(data[i + 1]) << 8;
            tail |= static_cast<u32>(data[i + 2]);
            break;
        case 2:
            tail |= static_cast<u32>(data[i]) << 8;
            tail |= static_cast<u32>(data[i + 1]);
            break;
        case 1:
            tail |= static_cast<u32>(data[i]);
            break;
        default:
            break;
    }
    return csum ^ tail;
}

// =============================================================================
// Container parsing
// =============================================================================

Result<Cabinet, Error> Cabinet::parse(std::span<const u8> image) {
    using R = Result<Cabinet, Error>;

    Cursor cur(image);
    if (!cur.has(kFixedHeaderSize)) return R::err(Error::InvalidFormat);

    if (!(image[0] == 'M' && image[1] == 'S' && image[2] == 'C' && image[3] == 'F')) {
        return R::err(Error::InvalidFormat);
    }
    cur.seek(4);

    Cabinet cab;
    cab.image_ = image;
    Header& h = cab.header_;

    (void)cur.u32_();               // reserved1, must be ignored not validated
    h.cabinet_size = cur.u32_();
    (void)cur.u32_();               // reserved2
    h.files_offset = cur.u32_();
    (void)cur.u32_();               // reserved3
    h.version_minor = cur.u8_();
    h.version_major = cur.u8_();
    h.folder_count = cur.u16_();
    h.file_count = cur.u16_();
    h.flags = cur.u16_();
    h.set_id = cur.u16_();
    h.cabinet_index = cur.u16_();

    if (h.folder_count == 0 || h.file_count == 0) return R::err(Error::InvalidFormat);
    // cbCabinet describes the whole image. A short buffer means a truncated
    // download or a bad slice; decoding it would report plausible garbage.
    if (h.cabinet_size > image.size()) return R::err(Error::InvalidFormat);

    if ((h.flags & kFlagReservePresent) != 0) {
        if (!cur.has(4)) return R::err(Error::InvalidFormat);
        h.header_reserve_size = cur.u16_();
        h.folder_reserve_size = cur.u8_();
        h.data_reserve_size = cur.u8_();
        if (h.header_reserve_size > kMaxHeaderReserve) return R::err(Error::InvalidFormat);
        if (!cur.has(h.header_reserve_size)) return R::err(Error::InvalidFormat);
        cur.seek(cur.offset() + h.header_reserve_size);
    }

    // The multi-cabinet name/disk pairs come after the reserve area, in flag
    // order: previous first, then next.
    if ((h.flags & kFlagPrevCabinet) != 0) {
        if (!cur.string_(h.previous_cabinet) || !cur.string_(h.previous_disk)) {
            return R::err(Error::InvalidFormat);
        }
    }
    if ((h.flags & kFlagNextCabinet) != 0) {
        if (!cur.string_(h.next_cabinet) || !cur.string_(h.next_disk)) {
            return R::err(Error::InvalidFormat);
        }
    }

    // --- CFFOLDER[] ---------------------------------------------------------
    const usize folder_stride = kFolderRecordSize + h.folder_reserve_size;
    cab.folders_.reserve(h.folder_count);
    for (u16 i = 0; i < h.folder_count; ++i) {
        if (!cur.has(folder_stride)) return R::err(Error::InvalidFormat);
        Folder f;
        f.data_offset = cur.u32_();
        f.data_block_count = cur.u16_();
        f.raw_compression = cur.u16_();
        cur.seek(cur.offset() + h.folder_reserve_size);

        const u16 type = static_cast<u16>(f.raw_compression & kCompressTypeMask);
        switch (type) {
            case 0: f.compression = CompressionType::None; break;
            case 1: f.compression = CompressionType::MsZip; break;
            case 2: f.compression = CompressionType::Quantum; break;
            case 3: f.compression = CompressionType::Lzx; break;
            // An unknown type is not decodable. Reporting it as stored data
            // would hand back compressed bytes that look like a valid file.
            default: return R::err(Error::InvalidFormat);
        }
        if (f.compression == CompressionType::Lzx) {
            f.lzx_window_bits = static_cast<u8>(
                (f.raw_compression & kCompressLzxWindowMask) >> kCompressLzxWindowShift);
        }
        if (f.data_offset >= image.size()) return R::err(Error::InvalidFormat);
        cab.folders_.push_back(f);
    }

    // --- CFFILE[] -----------------------------------------------------------
    // coffFiles is authoritative; the file list is not required to sit
    // immediately after the folder list.
    if (h.files_offset >= image.size()) return R::err(Error::InvalidFormat);
    Cursor files(image, h.files_offset);
    cab.files_.reserve(h.file_count);
    for (u16 i = 0; i < h.file_count; ++i) {
        if (!files.has(kFileRecordSize)) return R::err(Error::InvalidFormat);
        FileEntry e;
        e.size = files.u32_();
        e.folder_offset = files.u32_();
        e.folder_index = files.u16_();
        e.date = files.u16_();
        e.time = files.u16_();
        e.attributes = files.u16_();
        if (!files.string_(e.name)) return R::err(Error::InvalidFormat);
        // Sentinel iFolder values are legal and preserved verbatim; a plain
        // index past the folder list is not.
        if (!e.spans_cabinets() && e.folder_index >= cab.folders_.size()) {
            return R::err(Error::InvalidFormat);
        }
        cab.files_.push_back(std::move(e));
    }

    return R::ok(std::move(cab));
}

Result<std::vector<DataBlock>, Error> Cabinet::enumerate_blocks(usize folder_index) const {
    using R = Result<std::vector<DataBlock>, Error>;
    if (folder_index >= folders_.size()) return R::err(Error::InvalidArgument);

    const Folder& folder = folders_[folder_index];
    const usize stride = kDataRecordSize + header_.data_reserve_size;

    std::vector<DataBlock> blocks;
    blocks.reserve(folder.data_block_count);

    Cursor cur(image_, folder.data_offset);
    for (u16 i = 0; i < folder.data_block_count; ++i) {
        if (!cur.has(stride)) return R::err(Error::InvalidFormat);
        DataBlock b;
        b.checksum = cur.u32_();
        b.compressed_size = cur.u16_();
        b.uncompressed_size = cur.u16_();
        cur.seek(cur.offset() + header_.data_reserve_size);
        b.payload_offset = cur.offset();
        if (!cur.has(b.compressed_size)) return R::err(Error::InvalidFormat);
        if (b.uncompressed_size > kMaxBlockUncompressed) return R::err(Error::InvalidFormat);
        cur.seek(cur.offset() + b.compressed_size);
        blocks.push_back(b);
    }
    return R::ok(std::move(blocks));
}

// =============================================================================
// Decompression
// =============================================================================

Result<std::vector<u8>, Error> Cabinet::extract_folder(usize folder_index) const {
    using R = Result<std::vector<u8>, Error>;
    if (folder_index >= folders_.size()) return R::err(Error::InvalidArgument);
    const Folder& folder = folders_[folder_index];

    auto blocks_result = enumerate_blocks(folder_index);
    if (blocks_result.is_err()) return R::err(blocks_result.error());
    const std::vector<DataBlock>& blocks = blocks_result.value();

    usize total = 0;
    for (const DataBlock& b : blocks) total += b.uncompressed_size;

    std::vector<u8> out;
    out.reserve(total);

    switch (folder.compression) {
        case CompressionType::None: {
            for (const DataBlock& b : blocks) {
                // Stored folders still carry both sizes, and they must agree:
                // a mismatch means the folder is not actually stored.
                if (b.compressed_size != b.uncompressed_size) return R::err(Error::InvalidFormat);
                const u8* src = image_.data() + b.payload_offset;
                out.insert(out.end(), src, src + b.compressed_size);
            }
            break;
        }
        case CompressionType::Lzx: {
            auto decoder_result = lzx::Decoder::create(folder.lzx_window_bits);
            if (decoder_result.is_err()) return R::err(decoder_result.error());
            lzx::Decoder decoder = std::move(decoder_result).value();

            // Strictly sequential: the LZX window, Huffman path lengths, and
            // the R0/R1/R2 LRU all carry from one CFDATA into the next, so
            // there is no way to start at block N.
            for (const DataBlock& b : blocks) {
                std::span<const u8> payload(image_.data() + b.payload_offset, b.compressed_size);
                auto n = decoder.decode_chunk(payload, b.uncompressed_size, out);
                if (n.is_err()) return R::err(n.error());
                if (n.value() != b.uncompressed_size) return R::err(Error::InvalidFormat);
            }
            break;
        }
        case CompressionType::MsZip: {
            // Every block is its own DEFLATE stream, but the 32 KB history
            // carries across the boundary, so the decoder is fed the folder's
            // chunks in order and holds that dictionary itself.
            mszip::Decoder decoder;
            for (const DataBlock& b : blocks) {
                std::span<const u8> payload(image_.data() + b.payload_offset, b.compressed_size);
                auto n = decoder.decode_chunk(payload, b.uncompressed_size, out);
                if (n.is_err()) return R::err(n.error());
                if (n.value() != b.uncompressed_size) return R::err(Error::InvalidFormat);
            }
            break;
        }
        case CompressionType::Quantum:
            // Fail honestly rather than returning the compressed bytes.
            return R::err(Error::NotSupported);
    }

    if (out.size() != total) return R::err(Error::InvalidFormat);
    return R::ok(std::move(out));
}

Result<std::span<const u8>, Error> Cabinet::slice_file(const FileEntry& file,
                                                      std::span<const u8> folder_data) {
    using R = Result<std::span<const u8>, Error>;
    const u64 end = static_cast<u64>(file.folder_offset) + file.size;
    if (end > folder_data.size()) return R::err(Error::InvalidFormat);
    return R::ok(folder_data.subspan(file.folder_offset, file.size));
}

Result<std::vector<u8>, Error> Cabinet::extract_file(const FileEntry& file) const {
    using R = Result<std::vector<u8>, Error>;
    // A continued file's bytes live partly in a neighbouring cabinet this
    // reader was not given. Returning the local part would be a truncated
    // file that no caller can distinguish from a complete one.
    if (file.spans_cabinets()) return R::err(Error::NotSupported);
    if (file.folder_index >= folders_.size()) return R::err(Error::InvalidArgument);

    auto folder_data = extract_folder(file.folder_index);
    if (folder_data.is_err()) return R::err(folder_data.error());

    auto slice = slice_file(file, folder_data.value());
    if (slice.is_err()) return R::err(slice.error());
    return R::ok(std::vector<u8>(slice.value().begin(), slice.value().end()));
}

Result<std::vector<u8>, Error> Cabinet::extract_file(usize file_index) const {
    if (file_index >= files_.size()) {
        return Result<std::vector<u8>, Error>::err(Error::InvalidArgument);
    }
    return extract_file(files_[file_index]);
}

Result<std::vector<u8>, Error> Cabinet::extract_file_by_name(std::string_view name) const {
    const FileEntry* entry = find_file(name);
    if (entry == nullptr) return Result<std::vector<u8>, Error>::err(Error::NotFound);
    return extract_file(*entry);
}

const FileEntry* Cabinet::find_file(std::string_view name) const {
    // Byte-exact comparison. CFFILE names are codepage bytes unless
    // _A_NAME_IS_UTF is set, so there is no case folding this layer can do
    // correctly without knowing the codepage — that belongs to the caller.
    for (const FileEntry& e : files_) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

}  // namespace vitreo::compression::cabinet
