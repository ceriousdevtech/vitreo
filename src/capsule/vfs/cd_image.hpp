#pragma once
/// @file cd_image.hpp
/// @brief A CD image as a drive sees it: table of contents plus sectors.
///
/// Formats:
///  - ISO 9660 (2048-byte sectors): one data track starting at LBA 0; the
///    lead-out follows the last sector.
///  - Raw MODE1/2352 (single-track CUE or bare .bin): one data track.
///  - Raw 2448-byte sectors (2352 main channel + 96 interleaved P-W
///    subchannel bytes, Alcohol's .mdf without its .mds): the table of
///    contents is rebuilt from the Q subchannel (ECMA-130 §22.3.3, mode-1
///    Q: control/ADR, track, index, relative and absolute MSF, CRC-16).
///    The absolute time of each frame is its true address, so frames the
///    dump left out (part of a pregap) are absent addresses, not a shift.
///
/// LBA 0 is absolute time 00:02:00 (MSF - 150), as in the TOC a drive
/// reports through MCI and IOCTL_CDROM_READ_TOC.

#include <vitreo/common.hpp>

#include <array>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace vitreo::capsule {

struct CdTrack {
    u8 number = 0;       ///< 1..99
    u8 control = 0;      ///< Q control nibble (bit 2: data track)
    u32 start_lba = 0;   ///< index 1
    u32 length = 0;      ///< sectors up to the next track or the lead-out
    [[nodiscard]] bool is_audio() const noexcept { return (control & 0x4) == 0; }
};

class CdImage {
public:
    static constexpr u32 kRawSectorSize = 2352;
    static constexpr u32 kFramesPerSecond = 75;
    static constexpr u32 kLbaToMsfOffset = 150;

    /// Open @p path (the data file itself). nullptr with @p error set when the
    /// file is no CD image this understands.
    [[nodiscard]] static std::unique_ptr<CdImage> open(const std::string& path,
                                                       std::string& error);

    [[nodiscard]] const std::vector<CdTrack>& tracks() const noexcept { return tracks_; }
    [[nodiscard]] u32 leadout_lba() const noexcept { return leadout_lba_; }
    /// True when the image carries the full 2352-byte frames (audio can be
    /// read); a 2048-byte ISO holds user data only.
    [[nodiscard]] bool has_raw_frames() const noexcept { return physical_size_ != 2048; }

    /// The 2352 bytes of frame @p lba; a frame the image does not hold (an
    /// omitted pregap) reads as digital silence. false past the lead-out or
    /// for an ISO image.
    [[nodiscard]] bool read_raw(u32 lba, std::span<u8> frame);
    /// The 2048 user-data bytes of data sector @p lba (mode 1 / ISO).
    [[nodiscard]] bool read_user_data(u32 lba, std::span<u8> sector);

    /// Q subchannel decode of one 96-byte interleaved P-W block: the 12 Q
    /// bytes, and whether its CRC holds. Exposed for tests.
    [[nodiscard]] static bool decode_q(std::span<const u8> subchannel,
                                       std::array<u8, 12>& q) noexcept;

private:
    /// A run of consecutive image frames holding consecutive addresses.
    struct Run {
        u32 image_index = 0;
        u32 lba = 0;
        u32 count = 0;
    };

    CdImage() = default;
    [[nodiscard]] bool load_subchannel_toc(u32 frame_count, std::string& error);
    [[nodiscard]] bool image_index_for(u32 lba, u32& index) const noexcept;
    [[nodiscard]] bool read_frame_bytes(u32 index, u32 offset, std::span<u8> out);

    std::ifstream file_;
    std::mutex mutex_;
    u32 physical_size_ = 2048;
    u32 user_data_offset_ = 0;
    std::vector<Run> runs_;
    std::vector<CdTrack> tracks_;
    u32 leadout_lba_ = 0;
};

} // namespace vitreo::capsule
