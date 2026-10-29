// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file com_test_audio_doubles.hpp
/// @brief Audio voice/renderer test doubles (test_com_*.cpp). Test-local.

#include "com_test_common.hpp"

class TestAudioVoice final : public audio::IAudioVoice {
public:
    bool play(bool) override { playing = true; return true; }
    void pause() override { playing = false; }
    void stop() override { playing = false; }
    bool is_playing() const override { return playing; }
    u32 play_cursor() const override { return 0; }
    u32 write_cursor() const override { return 0; }
    void set_position(u32) override {}
    void set_volume(i32) override {}
    void set_pan(i32) override {}
    void set_frequency(u32) override {}
    bool playing{false};
};

class TestAudioPushVoice final : public audio::IAudioPushVoice {
public:
    TestAudioPushVoice(const audio::PcmFormat& f, bool adjustable)
        : format(f), block_align(f.block_align()), rate_adjust(adjustable),
          gains(f.channels, 1.0f) {}
    bool start() override { started = true; return true; }
    void pause() override { started = false; }
    void reset() override { padding = 0; total = 0; submitted.clear(); }
    u32 padding_frames() const override { return padding; }
    bool submit(const u8* frames, u32 count, bool silent) override {
        submitted.resize(static_cast<usize>(count) * block_align);
        if (silent) std::fill(submitted.begin(), submitted.end(), 0u);
        else std::copy_n(frames, submitted.size(), submitted.begin());
        padding += count;
        total += count;
        return true;
    }
    /// Tests set `padding` to model the engine; passes are only counted.
    void engine_pass(u64) override { passes.fetch_add(1); }
    /// A device with no latency of its own: what the engine read has played.
    u64 played_frames() const override { return total - padding; }
    u32 latency_frames() const override { return 0; }
    void set_channel_gains(const float* values, u32 count) override {
        gains.assign(values, values + count);
    }
    bool set_rate_ratio(double ratio) override {
        // The CoreAudio voice's play-rate range.
        if (!rate_adjust || !(ratio >= 0.5 && ratio <= 2.0)) return false;
        rate_ratio = ratio;
        return true;
    }
    /// The last packet as the device takes it: through the same gain stage
    /// the CoreAudio voice applies when it dequeues frames.
    [[nodiscard]] std::vector<u8> rendered() const {
        std::vector<u8> out = submitted;
        audio::apply_channel_gains(format, out.data(),
                                   static_cast<u32>(out.size() / block_align), gains.data());
        return out;
    }
    audio::PcmFormat format{};
    u32 block_align{0};
    bool rate_adjust{false};
    std::vector<float> gains;
    double rate_ratio{1.0};
    u32 padding{0};
    u64 total{0};
    std::atomic<u64> passes{0};
    bool started{false};
    std::vector<u8> submitted;
};

class TestAudioRenderer final : public audio::IAudioRenderer {
public:
    std::unique_ptr<audio::IAudioVoice> create_voice(
        const audio::PcmFormat& format, const u8* ring, u32 bytes) override {
        last_format = format;
        last_ring = ring;
        last_bytes = bytes;
        return std::make_unique<TestAudioVoice>();
    }
    std::unique_ptr<audio::IAudioPushVoice> create_push_voice(
        const audio::PcmFormat& format, u32, bool rate_adjust) override {
        auto voice = std::make_unique<TestAudioPushVoice>(format, rate_adjust);
        last_push = voice.get();
        last_format = format;
        last_bytes = 0;
        return voice;
    }
    audio::PcmFormat last_format{};
    const u8* last_ring{nullptr};
    u32 last_bytes{0};
    TestAudioPushVoice* last_push{nullptr};
};
