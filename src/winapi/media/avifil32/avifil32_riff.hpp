#pragma once

/// @file avifil32_riff.hpp
/// @brief Host-side model of the files avifil32's built-in handlers read:
/// the AVI file handler (CLSID_AVIFile) and the WAVE file handler.
///
/// Layouts: Learn "AVI RIFF File Reference" and vfw.h (MainAVIHeader,
/// AVIStreamHeader, AVIINDEXENTRY, AVIPALCHANGE). The model keeps what the
/// handler exposes, as measured on Windows (tools/oracle_probe/
/// avifil32_probe*.expected):
///  - The index is idx1 when present (offsets absolute or relative to the
///    'movi' list), else a walk of the first RIFF's 'movi' list including
///    the 'rec ' lists. Windows' handler ignores OpenDML (indx / ix## /
///    RIFF 'AVIX'): those frames read as missing, as there.
///  - idx1: a chunk is a key frame when AVIIF_KEYFRAME is set or its
///    two-character code is 'db' (uncompressed DIB). Scanned: only each
///    stream's first chunk is a key frame.
///  - Unknown chunks inside 'hdrl' and top-level 'LIST' chunks other than
///    'movi' are file data (AVIFileReadData; a LIST keeps its type), unknown
///    chunks in 'strl' are stream data (AVIStreamReadData); strd, strn,
///    indx and JUNK are not.

#include <vitreo/common.hpp>

#include <memory>
#include <string>
#include <vector>

namespace vitreo::winapi::avifil32 {

/// Random access to a file's bytes.
class ByteSource {
public:
    virtual ~ByteSource() = default;
    [[nodiscard]] virtual u64 size() const = 0;
    /// Read exactly @p n bytes at @p offset; false when they are not there.
    [[nodiscard]] virtual bool read(u64 offset, void* dst, usize n) = 0;
};

class MemorySource final : public ByteSource {
public:
    explicit MemorySource(std::vector<u8> bytes) : bytes_(std::move(bytes)) {}
    [[nodiscard]] u64 size() const override { return bytes_.size(); }
    [[nodiscard]] bool read(u64 offset, void* dst, usize n) override;

private:
    std::vector<u8> bytes_;
};

struct DataChunk {
    u32 ckid{0};
    std::vector<u8> data;
};

/// One chunk of one stream's data, in index order.
struct StreamChunk {
    u64 offset{0};      ///< file offset of the chunk's data
    u32 size{0};        ///< bytes
    i64 first{0};       ///< stream position of its first sample
    u32 samples{0};     ///< samples it holds (1 for variable-size streams)
    u32 ordinal{0};     ///< position in the file index (FIND_INDEX)
    bool key{false};
};

/// A '##pc' palette change: applies from the sample at @p pos on.
struct PaletteChange {
    i64 pos{0};
    u32 ordinal{0};
    std::vector<u8> data; ///< AVIPALCHANGE
};

struct AviStreamHeader {
    u32 fcc_type{0};
    u32 fcc_handler{0};
    u32 flags{0};
    u16 priority{0};
    u16 language{0};
    u32 initial_frames{0};
    u32 scale{0};
    u32 rate{0};
    u32 start{0};
    u32 length{0};
    u32 suggested_buffer{0};
    u32 quality{0};
    u32 sample_size{0};
    i16 frame[4]{};
};

struct AviStream {
    AviStreamHeader header;
    std::vector<u8> format;
    std::vector<DataChunk> extra;
    bool has_name{false};
    std::string name; ///< strn bytes (CP_ACP), without the terminator
    std::vector<StreamChunk> chunks;
    std::vector<PaletteChange> palette_changes;
    u64 total_samples{0}; ///< samples present in the index
};

struct AviMainHeader {
    u32 usec_per_frame{0};
    u32 max_bytes_per_sec{0};
    u32 padding{0};
    u32 flags{0};
    u32 total_frames{0};
    u32 initial_frames{0};
    u32 streams{0};
    u32 suggested_buffer{0};
    u32 width{0};
    u32 height{0};
};

struct AviFileModel {
    AviMainHeader header;
    std::vector<AviStream> streams;
    std::vector<DataChunk> extra;
};

struct WaveFileModel {
    std::vector<u8> format; ///< 'fmt ' chunk (WAVEFORMAT[EX])
    u64 data_offset{0};
    u32 data_size{0};
    u16 block_align{0};
    u32 avg_bytes_per_sec{0};
    u32 samples_per_sec{0};
};

/// Parse an AVI file. Returns AVIERR_OK, AVIERR_BADFORMAT (not a RIFF 'AVI '
/// file or no usable header) or AVIERR_FILEREAD.
[[nodiscard]] u32 parse_avi(ByteSource& src, AviFileModel& out);

/// Parse a RIFF 'WAVE' file. Returns AVIERR_OK or AVIERR_BADFORMAT.
[[nodiscard]] u32 parse_wave(ByteSource& src, WaveFileModel& out);

/// The RIFF form type of a file ('AVI ', 'WAVE'), or 0 when the file does
/// not start with a RIFF header.
[[nodiscard]] u32 riff_form_type(ByteSource& src);

} // namespace vitreo::winapi::avifil32
