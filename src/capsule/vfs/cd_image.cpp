/// @file cd_image.cpp
/// @brief CD image table of contents and sector access (see cd_image.hpp).

#include "capsule/vfs/cd_image.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>

namespace vitreo::capsule {

namespace {

constexpr u32 kSubchannelSize = 96;
constexpr u32 kSubchannelFrameSize = CdImage::kRawSectorSize + kSubchannelSize;  // 2448
constexpr u8 kSync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
/// Frames between Q samples; transitions are then located exactly by
/// bisection. Along a disc the Q key (track, index, address offset) only
/// moves forward, so a key never reappears after a different one and a
/// sparse sample cannot hide a stretch.
constexpr u32 kSampleStep = 4096;
/// How far past a frame with a damaged Q (CRC fails) to look for a good one.
constexpr u32 kQSearch = 32;

u16 crc16_ccitt(std::span<const u8> bytes) noexcept {
    u16 crc = 0;
    for (const u8 b : bytes) {
        crc ^= static_cast<u16>(b) << 8;
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 0x8000) ? static_cast<u16>((crc << 1) ^ 0x1021)
                                 : static_cast<u16>(crc << 1);
        }
    }
    return crc;
}

int from_bcd(u8 v) noexcept { return (v >> 4) * 10 + (v & 0x0F); }
bool is_bcd(u8 v) noexcept { return (v >> 4) <= 9 && (v & 0x0F) <= 9; }

/// What a mode-1 Q frame says about the frame carrying it.
struct QKey {
    u8 control = 0;
    u8 track = 0;
    u8 index = 0;
    i64 offset = 0;  ///< address minus image frame index
    bool operator==(const QKey&) const = default;
};

} // namespace

bool CdImage::decode_q(std::span<const u8> subchannel, std::array<u8, 12>& q) noexcept {
    if (subchannel.size() < kSubchannelSize) return false;
    // Interleaved P-W: bit 6 of each of the 96 bytes is the Q channel.
    for (u32 i = 0; i < 12; ++i) {
        u8 byte = 0;
        for (u32 bit = 0; bit < 8; ++bit) {
            byte = static_cast<u8>((byte << 1) | ((subchannel[i * 8 + bit] >> 6) & 1));
        }
        q[i] = byte;
    }
    const u16 stored = static_cast<u16>((q[10] << 8) | q[11]);
    return static_cast<u16>(crc16_ccitt(std::span<const u8>(q.data(), 10)) ^ 0xFFFF) == stored;
}

std::unique_ptr<CdImage> CdImage::open(const std::string& path, std::string& error) {
    std::unique_ptr<CdImage> image(new CdImage());
    image->file_.open(path, std::ios::binary);
    if (!image->file_) {
        error = "Unable to open image data";
        return nullptr;
    }
    std::error_code ec;
    const u64 size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "Unable to size image data";
        return nullptr;
    }
    const auto signature_at = [&](u64 offset) {
        char sig[5]{};
        image->file_.clear();
        image->file_.seekg(static_cast<std::streamoff>(offset + 1));
        image->file_.read(sig, 5);
        return image->file_.gcount() == 5 && std::memcmp(sig, "CD001", 5) == 0;
    };
    const auto sync_at = [&](u64 offset) {
        u8 sync[12]{};
        image->file_.clear();
        image->file_.seekg(static_cast<std::streamoff>(offset));
        image->file_.read(reinterpret_cast<char*>(sync), 12);
        return image->file_.gcount() == 12 && std::memcmp(sync, kSync, 12) == 0;
    };
    const auto single_data_track = [&](u32 frames) {
        CdTrack track;
        track.number = 1;
        track.control = 0x4;
        track.start_lba = 0;
        track.length = frames;
        image->tracks_ = {track};
        image->runs_ = {Run{0, 0, frames}};
        image->leadout_lba_ = frames;
    };

    if (size % kSubchannelFrameSize == 0 && size >= 17ull * kSubchannelFrameSize &&
        sync_at(0) && signature_at(16ull * kSubchannelFrameSize + 16)) {
        image->physical_size_ = kSubchannelFrameSize;
        image->user_data_offset_ = 16;
        if (!image->load_subchannel_toc(static_cast<u32>(size / kSubchannelFrameSize), error)) {
            return nullptr;
        }
        return image;
    }
    if (signature_at(16ull * kRawSectorSize + 16)) {
        image->physical_size_ = kRawSectorSize;
        image->user_data_offset_ = 16;
        single_data_track(static_cast<u32>(size / kRawSectorSize));
        return image;
    }
    if (signature_at(16ull * 2048)) {
        image->physical_size_ = 2048;
        image->user_data_offset_ = 0;
        single_data_track(static_cast<u32>(size / 2048));
        return image;
    }
    error = "Image is not an ISO9660, MODE1/2352 or 2448-byte raw CD image";
    return nullptr;
}

bool CdImage::load_subchannel_toc(u32 frame_count, std::string& error) {
    std::vector<u8> sub(kSubchannelSize);
    // The mode-1 Q at or just after @p index (frames whose Q is damaged or
    // carries ADR 2/3 are skipped).
    const auto key_at = [&](u32 index) -> std::optional<std::pair<u32, QKey>> {
        for (u32 i = index; i < frame_count && i < index + kQSearch; ++i) {
            if (!read_frame_bytes(i, kRawSectorSize, sub)) return std::nullopt;
            std::array<u8, 12> q{};
            if (!decode_q(sub, q)) continue;
            if ((q[0] & 0x0F) != 1) continue;          // ADR 1: position
            if (q[1] == 0xAA || q[1] == 0) continue;    // lead-out / lead-in
            if (!is_bcd(q[1]) || !is_bcd(q[2]) || !is_bcd(q[7]) || !is_bcd(q[8]) ||
                !is_bcd(q[9])) {
                continue;
            }
            const i64 msf = from_bcd(q[7]) * 60ll * kFramesPerSecond +
                            from_bcd(q[8]) * static_cast<i64>(kFramesPerSecond) + from_bcd(q[9]);
            QKey key;
            key.control = static_cast<u8>(q[0] >> 4);
            key.track = static_cast<u8>(from_bcd(q[1]));
            key.index = static_cast<u8>(from_bcd(q[2]));
            key.offset = msf - kLbaToMsfOffset - static_cast<i64>(i);
            return std::make_pair(i, key);
        }
        return std::nullopt;
    };

    // First frame of every stretch with one QKey: sample, then bisect each
    // interval whose ends disagree.
    std::map<u32, QKey> starts;
    std::optional<std::pair<u32, QKey>> prev;
    std::vector<std::pair<std::pair<u32, QKey>, std::pair<u32, QKey>>> pending;
    // The last good Q of the image closes the sample, so a track that starts
    // after the final sample point is still found.
    const auto key_before_end = [&]() -> std::optional<std::pair<u32, QKey>> {
        for (u32 back = 0; back < frame_count; back += kQSearch) {
            const u32 at = frame_count > back + kQSearch ? frame_count - back - kQSearch : 0;
            if (auto k = key_at(at)) return k;
            if (at == 0) break;
        }
        return std::nullopt;
    };
    std::vector<u32> sample_points;
    for (u32 i = 0; i < frame_count; i += kSampleStep) sample_points.push_back(i);
    if (auto tail = key_before_end()) sample_points.push_back(tail->first);
    for (const u32 i : sample_points) {
        auto cur = key_at(i);
        if (!cur) continue;
        if (prev && cur->first <= prev->first) continue;
        if (!prev) {
            starts[cur->first] = cur->second;
        } else if (!(prev->second == cur->second)) {
            pending.emplace_back(*prev, *cur);
        }
        prev = cur;
    }
    if (!prev) {
        error = "No readable Q subchannel in the image";
        return false;
    }
    while (!pending.empty()) {
        auto [lo, hi] = pending.back();
        pending.pop_back();
        if (hi.first <= lo.first + 1) {
            starts[hi.first] = hi.second;
            continue;
        }
        const u32 mid = lo.first + (hi.first - lo.first) / 2;
        auto m = key_at(mid);
        if (!m || m->first >= hi.first) {
            starts[hi.first] = hi.second;
            continue;
        }
        if (!(m->second == lo.second)) pending.emplace_back(lo, *m);
        if (!(m->second == hi.second)) pending.emplace_back(*m, hi);
    }

    runs_.clear();
    std::map<u8, std::pair<u32, u8>> track_start;  // track -> (lba, control)
    for (auto it = starts.begin(); it != starts.end(); ++it) {
        const u32 first = it->first;
        const auto next = std::next(it);
        const u32 end = next == starts.end() ? frame_count : next->first;
        const QKey& key = it->second;
        const i64 lba = static_cast<i64>(first) + key.offset;
        if (lba < 0) continue;  // pregap of track 1 (negative addresses)
        if (!runs_.empty() && runs_.back().lba + runs_.back().count == static_cast<u32>(lba) &&
            runs_.back().image_index + runs_.back().count == first) {
            runs_.back().count += end - first;
        } else {
            runs_.push_back(Run{first, static_cast<u32>(lba), end - first});
        }
        if (key.index >= 1 && !track_start.contains(key.track)) {
            track_start[key.track] = {static_cast<u32>(lba), key.control};
        }
    }
    if (runs_.empty() || track_start.empty()) {
        error = "Q subchannel names no track";
        return false;
    }
    leadout_lba_ = runs_.back().lba + runs_.back().count;
    tracks_.clear();
    for (const auto& [number, start] : track_start) {
        CdTrack track;
        track.number = number;
        track.start_lba = start.first;
        track.control = start.second;
        tracks_.push_back(track);
    }
    for (usize i = 0; i < tracks_.size(); ++i) {
        const u32 end = i + 1 < tracks_.size() ? tracks_[i + 1].start_lba : leadout_lba_;
        if (end < tracks_[i].start_lba) {
            error = "Q subchannel track addresses are out of order";
            return false;
        }
        tracks_[i].length = end - tracks_[i].start_lba;
    }
    return true;
}

bool CdImage::image_index_for(u32 lba, u32& index) const noexcept {
    const auto it = std::upper_bound(runs_.begin(), runs_.end(), lba,
                                     [](u32 value, const Run& run) { return value < run.lba; });
    if (it == runs_.begin()) return false;
    const Run& run = *std::prev(it);
    if (lba >= run.lba + run.count) return false;
    index = run.image_index + (lba - run.lba);
    return true;
}

bool CdImage::read_frame_bytes(u32 index, u32 offset, std::span<u8> out) {
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(static_cast<u64>(index) * physical_size_ + offset));
    file_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return file_.gcount() == static_cast<std::streamsize>(out.size());
}

bool CdImage::read_raw(u32 lba, std::span<u8> frame) {
    if (frame.size() < kRawSectorSize || physical_size_ == 2048 || lba >= leadout_lba_) {
        return false;
    }
    std::lock_guard lock(mutex_);
    u32 index = 0;
    if (!image_index_for(lba, index)) {
        std::fill(frame.begin(), frame.begin() + kRawSectorSize, u8{0});
        return true;
    }
    return read_frame_bytes(index, 0, frame.first(kRawSectorSize));
}

bool CdImage::read_user_data(u32 lba, std::span<u8> sector) {
    if (sector.size() < 2048 || lba >= leadout_lba_) return false;
    std::lock_guard lock(mutex_);
    u32 index = 0;
    if (!image_index_for(lba, index)) return false;
    return read_frame_bytes(index, user_data_offset_, sector.first(2048));
}

} // namespace vitreo::capsule
