#pragma once
/// @file cd_image_fixtures.hpp
/// @brief Synthetic CD images for the CdImage / IsoFileSystem / MCI tests.
///
/// make_iso: an ISO 9660 image of exactly @p sectors 2048-byte sectors (a
/// PVD, a terminator and a root directory), like the probe discs whose
/// cdaudio answers tests/oracle_data/mci_cdaudio_probe_*.expected records
/// (1491 and 4420 sectors).
///
/// make_mixed_mode_mdf: a 2448-byte-per-frame raw image (2352 main + 96
/// interleaved P-W subchannel, ECMA-130 Q mode 1 with CRC) holding a data
/// track 1 whose user data is make_iso's, then the first frame of track 2's
/// pregap (index 0), then, after @p omitted addresses the dump left out,
/// track 2 (audio) whose samples are a counter so reads can be checked.

#include <vitreo/common.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>
#include <vector>

namespace vitreo::test_cd {

inline void le32(u8* out, u32 v) {
    out[0] = static_cast<u8>(v); out[1] = static_cast<u8>(v >> 8);
    out[2] = static_cast<u8>(v >> 16); out[3] = static_cast<u8>(v >> 24);
}
inline void be32(u8* out, u32 v) {
    out[0] = static_cast<u8>(v >> 24); out[1] = static_cast<u8>(v >> 16);
    out[2] = static_cast<u8>(v >> 8); out[3] = static_cast<u8>(v);
}

/// The 2048-byte user data of every sector of a minimal ISO of @p sectors.
inline std::vector<u8> iso_bytes(u32 sectors, std::string_view label) {
    std::vector<u8> image(static_cast<usize>(sectors) * 2048, 0);
    u8* pvd = image.data() + 16 * 2048;
    pvd[0] = 1;
    std::copy_n("CD001", 5, pvd + 1);
    pvd[6] = 1;
    std::fill(pvd + 40, pvd + 72, ' ');
    std::copy(label.begin(), label.end(), pvd + 40);
    le32(pvd + 80, sectors);
    be32(pvd + 84, sectors);
    u8* root = pvd + 156;
    root[0] = 34;
    le32(root + 2, 18); be32(root + 6, 18);
    le32(root + 10, 2048); be32(root + 14, 2048);
    root[25] = 2; root[28] = 1; root[31] = 1; root[32] = 1; root[33] = 0;
    u8* term = image.data() + 17 * 2048;
    term[0] = 255;
    std::copy_n("CD001", 5, term + 1);
    term[6] = 1;
    u8* dir = image.data() + 18 * 2048;
    for (int i = 0; i < 2; ++i) {
        u8* r = dir + i * 34;
        r[0] = 34; le32(r + 2, 18); be32(r + 6, 18); le32(r + 10, 2048); be32(r + 14, 2048);
        r[25] = 2; r[28] = 1; r[31] = 1; r[32] = 1; r[33] = static_cast<u8>(i);
    }
    return image;
}

inline std::filesystem::path make_iso(const std::string& name, u32 sectors,
                                      std::string_view label = "VITREO_CD") {
    const auto path = std::filesystem::temp_directory_path() / name;
    const auto bytes = iso_bytes(sectors, label);
    std::ofstream(path, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

inline u8 bcd(u32 v) { return static_cast<u8>(((v / 10) << 4) | (v % 10)); }

inline u16 crc16(std::span<const u8> b) {
    u16 c = 0;
    for (u8 x : b) {
        c ^= static_cast<u16>(x) << 8;
        for (int i = 0; i < 8; ++i) c = (c & 0x8000) ? static_cast<u16>((c << 1) ^ 0x1021) : static_cast<u16>(c << 1);
    }
    return c;
}

/// One 96-byte interleaved subchannel block carrying a mode-1 Q frame.
inline std::array<u8, 96> q_block(u8 control, u32 track, u32 index, u32 rel_frames,
                                  u32 abs_frames, bool corrupt = false) {
    std::array<u8, 12> q{};
    q[0] = static_cast<u8>((control << 4) | 1);
    q[1] = bcd(track);
    q[2] = bcd(index);
    q[3] = bcd(rel_frames / 4500); q[4] = bcd((rel_frames / 75) % 60); q[5] = bcd(rel_frames % 75);
    q[7] = bcd(abs_frames / 4500); q[8] = bcd((abs_frames / 75) % 60); q[9] = bcd(abs_frames % 75);
    const u16 crc = static_cast<u16>(crc16(std::span<const u8>(q.data(), 10)) ^ 0xFFFF);
    q[10] = static_cast<u8>(crc >> 8);
    q[11] = static_cast<u8>(crc);
    if (corrupt) q[11] ^= 0x5A;
    std::array<u8, 96> sub{};
    for (u32 i = 0; i < 96; ++i) {
        const u8 bit = (q[i / 8] >> (7 - (i % 8))) & 1;
        sub[i] = static_cast<u8>(bit << 6);
    }
    return sub;
}

struct MixedLayout {
    u32 data_frames = 600;   ///< track 1, LBA 0..data_frames-1
    u32 omitted = 149;       ///< pregap addresses missing from the image
    u32 audio_frames = 900;  ///< track 2
};

/// Audio sample i of track 2 (16-bit little-endian, both channels).
inline u16 audio_sample(u32 frame, u32 i) { return static_cast<u16>(frame * 7 + i); }

inline std::filesystem::path make_mixed_mode_mdf(const std::string& name, MixedLayout layout,
                                                 std::string_view label = "MIXEDMODE") {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const auto data = iso_bytes(layout.data_frames, label);
    std::vector<u8> frame(2448);
    // Track 1, data, mode 1.
    for (u32 lba = 0; lba < layout.data_frames; ++lba) {
        std::fill(frame.begin(), frame.end(), u8{0});
        static constexpr u8 sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
        std::copy(std::begin(sync), std::end(sync), frame.begin());
        const u32 abs = lba + 150;
        frame[12] = bcd(abs / 4500); frame[13] = bcd((abs / 75) % 60); frame[14] = bcd(abs % 75);
        frame[15] = 1;
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(lba) * 2048, 2048, frame.begin() + 16);
        const auto sub = q_block(4, 1, 1, lba, abs, /*corrupt=*/lba % 97 == 13);
        std::copy(sub.begin(), sub.end(), frame.begin() + 2352);
        out.write(reinterpret_cast<const char*>(frame.data()), 2448);
    }
    // Track 2 pregap: only its first address (index 0) is in the image.
    {
        std::fill(frame.begin(), frame.end(), u8{0});
        const u32 lba = layout.data_frames;
        const auto sub = q_block(0, 2, 0, 150, lba + 150);
        std::copy(sub.begin(), sub.end(), frame.begin() + 2352);
        out.write(reinterpret_cast<const char*>(frame.data()), 2448);
    }
    // Track 2, audio, after the omitted addresses.
    const u32 start = layout.data_frames + 1 + layout.omitted;
    for (u32 i = 0; i < layout.audio_frames; ++i) {
        for (u32 s = 0; s < 588 * 2; ++s) {
            const u16 v = audio_sample(i, s);
            frame[s * 2] = static_cast<u8>(v);
            frame[s * 2 + 1] = static_cast<u8>(v >> 8);
        }
        const u32 lba = start + i;
        const auto sub = q_block(0, 2, 1, i, lba + 150);
        std::copy(sub.begin(), sub.end(), frame.begin() + 2352);
        out.write(reinterpret_cast<const char*>(frame.data()), 2448);
    }
    return path;
}

} // namespace vitreo::test_cd
