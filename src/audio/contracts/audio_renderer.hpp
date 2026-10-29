#pragma once
/// @file audio_renderer.hpp
/// @brief Frontend-neutral streaming PCM playback contract.
///
/// Mirrors the split used by the graphics subsystem: this header describes the
/// behavior an audio frontend (DirectSound, WinMM, XAudio) needs, and contains
/// no API-specific or platform-specific types. Backends live under
/// `src/audio/<backend>/`.

#include <vitreo/common.hpp>

#include <cstring>
#include <memory>

namespace vitreo::audio {

/// Linear PCM stream description.
struct PcmFormat {
    u32 sample_rate{0};
    u16 channels{0};
    /// Container size of one sample. Integer samples narrower than their
    /// container (WAVEFORMATEXTENSIBLE wValidBitsPerSample) are left-justified,
    /// so they play correctly as full-width samples.
    u16 bits_per_sample{0};
    /// IEEE-754 float samples (WAVE_FORMAT_IEEE_FLOAT /
    /// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) rather than integer PCM.
    bool float_samples{false};
    /// WAVEFORMATEXTENSIBLE dwChannelMask (SPEAKER_* bits), or 0 when the
    /// format did not assign speaker positions.
    u32 channel_mask{0};

    [[nodiscard]] u32 block_align() const noexcept {
        return static_cast<u32>(channels) * (static_cast<u32>(bits_per_sample) / 8u);
    }

    /// The legacy ring-buffer voice (DirectSound, WinMM): 8/16-bit integer
    /// mono or stereo, which is all WAVE_FORMAT_PCM carries.
    [[nodiscard]] bool valid() const noexcept {
        return !float_samples &&
               sample_rate >= 100 && sample_rate <= 200000 &&
               (channels == 1 || channels == 2) &&
               (bits_per_sample == 8 || bits_per_sample == 16);
    }

    /// A push stream (WASAPI shared mode): 8/16/24/32-bit integer or 32-bit
    /// float samples, up to `max_channels` channels (eight for the render
    /// voice; capture streams carry the input device's channels).
    [[nodiscard]] bool valid_stream(u16 max_channels = 8) const noexcept {
        const bool sample_ok = float_samples
            ? bits_per_sample == 32
            : (bits_per_sample == 8 || bits_per_sample == 16 ||
               bits_per_sample == 24 || bits_per_sample == 32);
        return sample_ok && sample_rate >= 100 && sample_rate <= 384000 &&
               channels >= 1 && channels <= max_channels;
    }
};

/// A voice plays a caller-owned ring buffer of PCM bytes.
///
/// This models the streaming contract every legacy Windows audio API exposes:
/// the guest owns the sample memory and writes into it continuously while the
/// device reads from it. The voice never copies the buffer up front, so guest
/// writes that land ahead of the play cursor become audible exactly as they do
/// on Windows. Byte offsets are relative to the start of that ring buffer.
class IAudioVoice {
public:
    virtual ~IAudioVoice() = default;

    /// Begin (or resume) playback from the current position.
    [[nodiscard]] virtual bool play(bool looping) = 0;

    /// Pause playback without discarding queued samples.
    virtual void pause() = 0;

    /// Stop playback, retaining the current position.
    virtual void stop() = 0;

    [[nodiscard]] virtual bool is_playing() const = 0;

    /// Byte offset the device has finished reading. Guest code compares this
    /// against its own write position to decide how much it may refill.
    [[nodiscard]] virtual u32 play_cursor() const = 0;

    /// First byte offset it is safe for the guest to write.
    [[nodiscard]] virtual u32 write_cursor() const = 0;

    /// Move the play position. Only meaningful while stopped or looping.
    virtual void set_position(u32 byte_offset) = 0;

    /// Attenuation in hundredths of a decibel, <= 0 (0 is full volume).
    virtual void set_volume(i32 hundredth_db) = 0;

    /// Stereo balance in hundredths of a decibel, negative is left.
    virtual void set_pan(i32 hundredth_db) = 0;

    /// Resample to a different playback rate (pitch shift).
    virtual void set_frequency(u32 sample_rate) = 0;
};

/// Cumulative push-voice health counters since creation (diagnostics).
struct PushVoiceCounters {
    u64 engine_passes{0};
    /// Engine passes that found less than their share of frames in the
    /// endpoint buffer: the client under-supplied (a guest-visible glitch).
    u64 short_passes{0};
    /// Times the host device ran out of queued frames.
    u64 device_dry{0};
};

/// A push-model PCM stream for APIs such as WASAPI shared mode, modelled as
/// an endpoint buffer the client writes and an engine that reads it.
///
/// submit() appends to the endpoint buffer (copied, so the frontend can
/// reuse its guest buffer at once). Frames leave the endpoint buffer only
/// when engine_pass() reads them into the host output path; padding is what
/// the engine has not read yet. Frames the engine has read are latency
/// (latency_frames()), not padding.
class IAudioPushVoice {
public:
    virtual ~IAudioPushVoice() = default;
    [[nodiscard]] virtual bool start() = 0;
    virtual void pause() = 0;
    virtual void reset() = 0;
    /// Frames in the endpoint buffer the engine has not read.
    [[nodiscard]] virtual u32 padding_frames() const = 0;
    [[nodiscard]] virtual bool submit(const u8* frames, u32 frame_count,
                                      bool silent) = 0;
    /// One engine processing pass covering @p elapsed_ns of engine time:
    /// read that many stream frames (at the stream rate times the rate
    /// ratio) from the endpoint buffer into the host output path. A shortfall
    /// plays as silence. No effect unless started.
    virtual void engine_pass(u64 elapsed_ns) = 0;
    /// Stream frames that have finished playing through the device since the
    /// voice was created or last reset; monotonic between resets.
    [[nodiscard]] virtual u64 played_frames() const = 0;
    /// Constant upper bound, in stream frames, on the time from an engine
    /// read of a frame until it plays through the device.
    [[nodiscard]] virtual u32 latency_frames() const = 0;
    [[nodiscard]] virtual PushVoiceCounters counters() const { return {}; }
    /// Linear amplitude gain per stream channel (0.0 to 1.0, `count` entries
    /// in channel order), multiplied into every frame the device takes from
    /// the stream from now on, including frames submitted earlier that have
    /// not been played yet. A new voice plays at unity gain.
    virtual void set_channel_gains(const float* gains, u32 count) = 0;
    /// Play the stream as though its frames were sampled at `ratio` times the
    /// format's rate (speed and pitch change together); engine passes read
    /// `ratio` times as many frames. Only a voice created
    /// with rate adjustment enabled accepts a ratio; false when it was not,
    /// or when `ratio` is outside the backend's supported range.
    [[nodiscard]] virtual bool set_rate_ratio(double ratio) = 0;
};

/// Scale interleaved frames in place by a linear per-channel gain (one entry
/// per channel, each 0.0 to 1.0). Unity entries leave their channel's samples
/// bit-identical; integer samples round to nearest.
inline void apply_channel_gains(const PcmFormat& format, u8* frames,
                                u32 frame_count, const float* gains) {
    const u32 channels = format.channels;
    const u32 bytes = format.bits_per_sample / 8u;
    for (u32 frame = 0; frame < frame_count; ++frame) {
        for (u32 channel = 0; channel < channels; ++channel) {
            const float gain = gains[channel];
            if (gain == 1.0f) continue;
            u8* sample = frames + (static_cast<usize>(frame) * channels + channel) * bytes;
            if (format.float_samples) {
                float value = 0.0f;
                std::memcpy(&value, sample, sizeof(value));
                value *= gain;
                std::memcpy(sample, &value, sizeof(value));
                continue;
            }
            switch (bytes) {
            case 1: {  // WAVE 8-bit PCM is unsigned, centred on 128.
                const double centred = (static_cast<double>(sample[0]) - 128.0) * gain;
                const double rounded = centred < 0 ? centred - 0.5 : centred + 0.5;
                sample[0] = static_cast<u8>(static_cast<i32>(rounded) + 128);
                break;
            }
            case 2: {
                i16 value = 0;
                std::memcpy(&value, sample, sizeof(value));
                const double scaled = static_cast<double>(value) * gain;
                value = static_cast<i16>(scaled < 0 ? scaled - 0.5 : scaled + 0.5);
                std::memcpy(sample, &value, sizeof(value));
                break;
            }
            case 3: {
                i32 value = static_cast<i32>(static_cast<u32>(sample[0]) |
                                             (static_cast<u32>(sample[1]) << 8u) |
                                             (static_cast<u32>(sample[2]) << 16u));
                if ((value & 0x800000) != 0) value -= 0x1000000;
                const double scaled = static_cast<double>(value) * gain;
                const i32 result = static_cast<i32>(scaled < 0 ? scaled - 0.5 : scaled + 0.5);
                sample[0] = static_cast<u8>(result & 0xFF);
                sample[1] = static_cast<u8>((result >> 8) & 0xFF);
                sample[2] = static_cast<u8>((result >> 16) & 0xFF);
                break;
            }
            case 4: {
                i32 value = 0;
                std::memcpy(&value, sample, sizeof(value));
                const double scaled = static_cast<double>(value) * gain;
                value = static_cast<i32>(scaled < 0 ? scaled - 0.5 : scaled + 0.5);
                std::memcpy(sample, &value, sizeof(value));
                break;
            }
            default: break;
            }
        }
    }
}

/// Device-level factory for voices.
class IAudioRenderer {
public:
    virtual ~IAudioRenderer() = default;

    /// Create a voice over `ring_bytes` of caller-owned memory at `ring`.
    /// The memory must outlive the returned voice.
    /// @return null when the format is unsupported or the device is unavailable
    [[nodiscard]] virtual std::unique_ptr<IAudioVoice> create_voice(
        const PcmFormat& format, const u8* ring, u32 ring_bytes) = 0;

    /// Create a push stream with the given Windows endpoint-buffer capacity.
    /// Accepts any PcmFormat::valid_stream() format. `rate_adjust` asks for a
    /// voice whose playback rate IAudioPushVoice::set_rate_ratio can change.
    /// Backends that do not support push playback return null.
    [[nodiscard]] virtual std::unique_ptr<IAudioPushVoice> create_push_voice(
        const PcmFormat&, u32, bool /*rate_adjust*/) { return {}; }
};

/// Create the platform audio renderer, or null when no device is available.
[[nodiscard]] std::unique_ptr<IAudioRenderer> create_audio_renderer();

} // namespace vitreo::audio
