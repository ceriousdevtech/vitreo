// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file com_wasapi_internal.hpp
/// @brief File-local MMDevice/WASAPI constants and wave-format helpers shared by
/// com_core.cpp (CoCreateInstance of the enumerator), wasapi_endpoints.cpp and
/// wasapi_audio_client.cpp. Everything sits in an anonymous namespace: each
/// including TU gets its own internal-linkage copy. Include from those three
/// files only.

#include "com_core.hpp"
#include "ole_guid_internal.hpp"
#include "winapi/core/marshaler.hpp"

#include <cmath>
#include <optional>

namespace vitreo::winapi {

namespace {

// Microsoft Learn / mmdeviceapi.h
constexpr Guid CLSID_MMDEVICEENUMERATOR{
    0xBCDE0395u, 0xE52Fu, 0x467Cu, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
constexpr Guid IID_IMMDEVICEENUMERATOR{
    0xA95664D2u, 0x9614u, 0x4F35u, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
constexpr Guid IID_IMMDEVICECOLLECTION{
    0x0BD7A1BEu, 0x7A1Au, 0x44DBu, {0x83, 0x97, 0xCC, 0x53, 0x92, 0x38, 0x7B, 0x5E}};
constexpr Guid IID_IMMDEVICE{
    0xD666063Fu, 0x1587u, 0x4E43u, {0x81, 0xF1, 0xB9, 0x48, 0xE8, 0x07, 0x36, 0x3F}};
constexpr Guid IID_IMMENDPOINT{
    0x1BE09788u, 0x6894u, 0x4089u, {0x85, 0x86, 0x9A, 0x2A, 0x6C, 0x26, 0x5A, 0xC5}};
constexpr Guid IID_IPROPERTYSTORE{
    0x886D8EEBu, 0x8CF2u, 0x4446u, {0x8D, 0x02, 0xCD, 0xBA, 0x1D, 0xBD, 0xCF, 0x99}};
constexpr Guid IID_IAUDIOCLIENT{
    0x1CB9AD4Cu, 0xDBFAu, 0x4C32u, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
constexpr Guid IID_IAUDIORENDERCLIENT{
    0xF294ACFCu, 0x3146u, 0x4483u, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
constexpr Guid PKEY_DEVICE_NAMES_FMTID{
    0xA45C254Eu, 0xDF1Cu, 0x4EFDu, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}};
// Learn (Core Audio device properties): PKEY_AudioEngine_DeviceFormat is
// {f19f064d-082c-4e27-bc73-6882a1bb8e4c},0 and carries the endpoint's format
// as a VT_BLOB holding a WAVEFORMATEX. A client that cannot read it has no
// channel count or sample rate to build a render pipeline with.
constexpr Guid PKEY_AUDIOENGINE_FMTID{
    0xF19F064Du, 0x082Cu, 0x4E27u, {0xBC, 0x73, 0x68, 0x82, 0xA1, 0xBB, 0x8E, 0x4C}};

/// The endpoint's device format, PKEY_AudioEngine_DeviceFormat. It must agree
/// with the mix format (MixWaveFormat, below) on sample rate and channel
/// count: a client reads the property to size its pipeline and then
/// initialises a shared-mode stream at that rate and channel count. The
/// sample representation may differ, as on Windows (integer device, float
/// engine).
struct EndpointWaveFormat {
    static constexpr u16 FORMAT_TAG = 1u;       // WAVE_FORMAT_PCM
    static constexpr u16 CHANNELS = 2u;
    static constexpr u32 SAMPLES_PER_SEC = 48000u;
    static constexpr u16 BITS_PER_SAMPLE = 16u;
    static constexpr u16 BLOCK_ALIGN = CHANNELS * (BITS_PER_SAMPLE / 8u);
    static constexpr u32 AVG_BYTES_PER_SEC = SAMPLES_PER_SEC * BLOCK_ALIGN;
    /// sizeof(WAVEFORMATEX) — 16 fixed bytes plus the cbSize field.
    static constexpr u32 BYTES = 18u;
};

/// Write a WAVEFORMATEX describing the endpoint at @p addr.
[[nodiscard]] inline bool write_endpoint_wave_format(Marshaler& m, GuestAddr addr) {
    using F = EndpointWaveFormat;
    return m.memory().write_u16(addr, F::FORMAT_TAG).is_ok() &&
           m.memory().write_u16(addr + 2, F::CHANNELS).is_ok() &&
           m.memory().write_u32(addr + 4, F::SAMPLES_PER_SEC).is_ok() &&
           m.memory().write_u32(addr + 8, F::AVG_BYTES_PER_SEC).is_ok() &&
           m.memory().write_u16(addr + 12, F::BLOCK_ALIGN).is_ok() &&
           m.memory().write_u16(addr + 14, F::BITS_PER_SAMPLE).is_ok() &&
           m.memory().write_u16(addr + 16, 0u).is_ok();
}

// mmreg.h / ksmedia.h (Windows SDK).
constexpr u16 WAVE_FORMAT_PCM = 0x0001u;
constexpr u16 WAVE_FORMAT_IEEE_FLOAT = 0x0003u;
constexpr u16 WAVE_FORMAT_EXTENSIBLE = 0xFFFEu;
constexpr Guid KSDATAFORMAT_SUBTYPE_PCM{
    0x00000001u, 0x0000u, 0x0010u, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr Guid KSDATAFORMAT_SUBTYPE_IEEE_FLOAT{
    0x00000003u, 0x0000u, 0x0010u, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
/// sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX): the cbSize an
/// extensible descriptor must carry.
constexpr u16 WAVEFORMATEXTENSIBLE_EXTRA = 22u;
constexpr u32 SPEAKER_FRONT_LEFT = 0x1u;
constexpr u32 SPEAKER_FRONT_RIGHT = 0x2u;

// audioclient.h / audiosessiontypes.h (Windows SDK). FACILITY_AUDCLNT is
// 0x889, so AUDCLNT_ERR(n) is 0x88890000 | n.
constexpr u32 AUDCLNT_E_NOT_INITIALIZED = 0x88890001u;
constexpr u32 AUDCLNT_E_ALREADY_INITIALIZED = 0x88890002u;
constexpr u32 AUDCLNT_E_DEVICE_INVALIDATED = 0x88890004u;
constexpr u32 AUDCLNT_E_NOT_STOPPED = 0x88890005u;
constexpr u32 AUDCLNT_E_BUFFER_TOO_LARGE = 0x88890006u;
constexpr u32 AUDCLNT_E_OUT_OF_ORDER = 0x88890007u;
constexpr u32 AUDCLNT_E_UNSUPPORTED_FORMAT = 0x88890008u;
constexpr u32 AUDCLNT_E_INVALID_SIZE = 0x88890009u;
constexpr u32 AUDCLNT_E_BUFFER_OPERATION_PENDING = 0x8889000Bu;
constexpr u32 AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED = 0x8889000Eu;
constexpr u32 AUDCLNT_E_ENDPOINT_CREATE_FAILED = 0x8889000Fu;
constexpr u32 AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED = 0x88890011u;
constexpr u32 AUDCLNT_E_EVENTHANDLE_NOT_SET = 0x88890014u;
constexpr u32 AUDCLNT_SHAREMODE_SHARED = 0u;
constexpr u32 AUDCLNT_SHAREMODE_EXCLUSIVE = 1u;
constexpr u32 AUDCLNT_STREAMFLAGS_CROSSPROCESS = 0x00010000u;
constexpr u32 AUDCLNT_STREAMFLAGS_LOOPBACK = 0x00020000u;
constexpr u32 AUDCLNT_STREAMFLAGS_EVENTCALLBACK = 0x00040000u;
constexpr u32 AUDCLNT_STREAMFLAGS_NOPERSIST = 0x00080000u;
constexpr u32 AUDCLNT_STREAMFLAGS_RATEADJUST = 0x00100000u;
constexpr u32 AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY = 0x08000000u;
constexpr u32 AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM = 0x80000000u;
constexpr u32 AUDCLNT_SESSIONFLAGS_EXPIREWHENUNOWNED = 0x10000000u;
constexpr u32 AUDCLNT_SESSIONFLAGS_DISPLAY_HIDE = 0x20000000u;
constexpr u32 AUDCLNT_SESSIONFLAGS_DISPLAY_HIDEWHENEXPIRED = 0x40000000u;
constexpr u32 AUDCLNT_KNOWN_INITIALIZE_FLAGS =
    AUDCLNT_STREAMFLAGS_CROSSPROCESS | AUDCLNT_STREAMFLAGS_LOOPBACK |
    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST |
    AUDCLNT_STREAMFLAGS_RATEADJUST | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_SESSIONFLAGS_EXPIREWHENUNOWNED |
    AUDCLNT_SESSIONFLAGS_DISPLAY_HIDE | AUDCLNT_SESSIONFLAGS_DISPLAY_HIDEWHENEXPIRED;
constexpr u32 AUDCLNT_BUFFERFLAGS_SILENT = 0x2u;

namespace capture_contract {
#include "winapi/com/ole32/wasapi_capture_contract.inc"
}  // namespace capture_contract
static_assert(capture_contract::AUDCLNT_E_INVALID_SIZE == AUDCLNT_E_INVALID_SIZE);
static_assert(capture_contract::AUDCLNT_E_OUT_OF_ORDER == AUDCLNT_E_OUT_OF_ORDER);
static_assert(capture_contract::AUDCLNT_E_DEVICE_INVALIDATED == AUDCLNT_E_DEVICE_INVALIDATED);

/// The shared-mode engine's period (IAudioClient::GetDevicePeriod), in
/// REFERENCE_TIME units: 10 ms default, 3 ms minimum — the values a Windows
/// shared-mode endpoint reports. Event-driven streams are signalled on the
/// default period.
constexpr u64 kEngineDefaultPeriodHns = 100000u;
constexpr u64 kEngineMinimumPeriodHns = 30000u;

/// The shared-mode mix format (IAudioClient::GetMixFormat). Learn: the method
/// "retrieves a format descriptor that is in the form of a
/// WAVEFORMATEXTENSIBLE structure instead of a standalone WAVEFORMATEX", and
/// the engine processes samples as floating point. A client that reads the
/// dwChannelMask or SubFormat of what it was handed must find them there, not
/// past the end of an 18-byte allocation.
struct MixWaveFormat {
    static constexpr u16 CHANNELS = 2u;
    static constexpr u32 SAMPLES_PER_SEC = 48000u;
    static constexpr u16 BITS_PER_SAMPLE = 32u;
    static constexpr u16 BLOCK_ALIGN = CHANNELS * (BITS_PER_SAMPLE / 8u);
    static constexpr u32 AVG_BYTES_PER_SEC = SAMPLES_PER_SEC * BLOCK_ALIGN;
    static constexpr u32 CHANNEL_MASK = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    /// sizeof(WAVEFORMATEXTENSIBLE).
    static constexpr u32 BYTES = 18u + WAVEFORMATEXTENSIBLE_EXTRA;
};
// The device format (PKEY_AudioEngine_DeviceFormat) may differ from the mix
// format in sample representation, but never in rate or channel count: the
// engine does not resample or remix between itself and its own device.
static_assert(MixWaveFormat::CHANNELS == EndpointWaveFormat::CHANNELS);
static_assert(MixWaveFormat::SAMPLES_PER_SEC == ComCoreManager::AUDIO_ENGINE_RATE);
static_assert(MixWaveFormat::SAMPLES_PER_SEC == EndpointWaveFormat::SAMPLES_PER_SEC);

/// Write the mix format's WAVEFORMATEXTENSIBLE at @p addr.
[[nodiscard]] inline bool write_mix_wave_format(Marshaler& m, GuestAddr addr) {
    using F = MixWaveFormat;
    return m.memory().write_u16(addr, WAVE_FORMAT_EXTENSIBLE).is_ok() &&
           m.memory().write_u16(addr + 2, F::CHANNELS).is_ok() &&
           m.memory().write_u32(addr + 4, F::SAMPLES_PER_SEC).is_ok() &&
           m.memory().write_u32(addr + 8, F::AVG_BYTES_PER_SEC).is_ok() &&
           m.memory().write_u16(addr + 12, F::BLOCK_ALIGN).is_ok() &&
           m.memory().write_u16(addr + 14, F::BITS_PER_SAMPLE).is_ok() &&
           m.memory().write_u16(addr + 16, WAVEFORMATEXTENSIBLE_EXTRA).is_ok() &&
           m.memory().write_u16(addr + 18, F::BITS_PER_SAMPLE).is_ok() &&
           m.memory().write_u32(addr + 20, F::CHANNEL_MASK).is_ok() &&
           m.write_struct(addr + 24, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT).is_ok();
}

/// The capture engine's rate. Vitreo's shared-mode capture engine runs at
/// 48 kHz whatever the host device's rate (the host converts), as Windows's
/// does over 44.1 kHz devices (audio_capture_contract_probe: device format
/// 44100 Hz, mix format 48000 Hz), so every engine figure (10 ms periods of
/// 480 frames, the 1056-frame buffer, the AUTOCONVERTPCM converter schedules)
/// is the measured one.
constexpr u32 kCaptureEngineRate = 48000;

/// A capture endpoint's shared-mode mix format: the engine rate and the
/// device's channels (with their speaker mask) as 32-bit float
/// WAVEFORMATEXTENSIBLE (measured on a stereo capture endpoint: tag 0xfffe,
/// 32 bits, cbSize 22, mask 3, IEEE float). The device format property
/// carries the device's own rate.
struct CaptureMixFormat {
    u32 rate{kCaptureEngineRate};
    u16 channels{2};
    u32 mask{SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT};
    [[nodiscard]] u16 block_align() const { return static_cast<u16>(channels * 4u); }
    [[nodiscard]] u32 channel_mask() const { return mask; }
};

[[nodiscard]] inline CaptureMixFormat capture_mix_format(const audio::CaptureDevice& device) {
    CaptureMixFormat f;
    f.rate = kCaptureEngineRate;
    f.channels = device.channels;
    f.mask = device.channel_mask;
    return f;
}

[[nodiscard]] inline CaptureMixFormat capture_device_format(const audio::CaptureDevice& device) {
    CaptureMixFormat f = capture_mix_format(device);
    f.rate = device.sample_rate;
    return f;
}

/// AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM's endpoint buffer for a zero-duration
/// stream at `rate` on the 48 kHz engine (measured at every probed rate,
/// tools/generate_audio_capture_contract.py checks the rule): the engine's
/// 1056 frames without a converter; otherwise the float32 rate ratio times
/// 1056, rounded up, plus 2, rounded up to an even count.
[[nodiscard]] inline u32 capture_minimum_buffer_frames(u32 rate) {
    const u32 engine = capture_contract::kCaptureMinimumBufferFrames48k;
    if (rate == kCaptureEngineRate) return engine;
    const float ratio = static_cast<float>(static_cast<double>(rate) / kCaptureEngineRate);
    const u64 scaled = static_cast<u64>(std::ceil(static_cast<double>(ratio) * engine));
    return static_cast<u32>((scaled + 2u + 1u) & ~u64{1});
}

/// The converter schedule measured for an AUTOCONVERTPCM stream at `rate`
/// (std::nullopt: never measured, so its packets cannot be reproduced).
[[nodiscard]] inline std::optional<audio::CapturePeriods> capture_periods_for(u32 rate,
                                                                       bool default_quality) {
    audio::CapturePeriods periods;
    periods.rate = rate;
    periods.period_frames = capture_contract::kCapturePacketFrames48k;
    if (rate == kCaptureEngineRate) return periods;  // no converter
    for (const auto& s : capture_contract::kMeasuredSrcSchedules) {
        if (s.rate == rate && s.default_quality == default_quality) {
            periods.delay_numerator = s.delay_numerator;
            periods.delay_denominator = s.delay_denominator;
            return periods;
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline bool write_capture_mix_format(Marshaler& m, GuestAddr addr,
                                            const CaptureMixFormat& f) {
    return m.memory().write_u16(addr, WAVE_FORMAT_EXTENSIBLE).is_ok() &&
           m.memory().write_u16(addr + 2, f.channels).is_ok() &&
           m.memory().write_u32(addr + 4, f.rate).is_ok() &&
           m.memory().write_u32(addr + 8, f.rate * f.block_align()).is_ok() &&
           m.memory().write_u16(addr + 12, f.block_align()).is_ok() &&
           m.memory().write_u16(addr + 14, 32u).is_ok() &&
           m.memory().write_u16(addr + 16, WAVEFORMATEXTENSIBLE_EXTRA).is_ok() &&
           m.memory().write_u16(addr + 18, 32u).is_ok() &&
           m.memory().write_u32(addr + 20, f.channel_mask()).is_ok() &&
           m.write_struct(addr + 24, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT).is_ok();
}

/// A guest WAVEFORMATEX / WAVEFORMATEXTENSIBLE, as read for a WASAPI stream.
struct StreamWaveFormat {
    u16 tag{0};
    u16 channels{0};
    u32 rate{0};
    u16 block_align{0};
    u16 bits{0};
    u16 extra{0};
    u16 valid_bits{0};
    u32 channel_mask{0};
    Guid subformat{};
};

enum class StreamFormatVerdict { Supported, Invalid, Unsupported };

/// Read and classify a stream format descriptor.
///   Invalid     — not a well-formed WAVEFORMATEX(TENSIBLE): E_INVALIDARG.
///   Unsupported — well formed, but not PCM or IEEE float the engine can mix:
///                 AUDCLNT_E_UNSUPPORTED_FORMAT.
/// On Supported, @p out describes the samples for the host voice. A capture
/// stream passes its limits: the device's channel ceiling and the lowest
/// AUTOCONVERTPCM client rate.
[[nodiscard]] inline StreamFormatVerdict read_stream_format(
    Marshaler& m, GuestAddr addr, StreamWaveFormat& raw, audio::PcmFormat& out,
    u16 max_channels = 8, u32 min_rate = 100) {
    auto& mem = m.memory();
    auto tag = mem.read_u16(addr);
    auto channels = mem.read_u16(addr + 2);
    auto rate = mem.read_u32(addr + 4);
    auto align = mem.read_u16(addr + 12);
    auto bits = mem.read_u16(addr + 14);
    if (tag.is_err() || channels.is_err() || rate.is_err() || align.is_err() ||
        bits.is_err()) {
        return StreamFormatVerdict::Invalid;
    }
    raw.tag = tag.value();
    raw.channels = channels.value();
    raw.rate = rate.value();
    raw.block_align = align.value();
    raw.bits = bits.value();
    raw.valid_bits = raw.bits;
    bool is_float = false;
    if (raw.tag == WAVE_FORMAT_EXTENSIBLE) {
        auto extra = mem.read_u16(addr + 16);
        if (extra.is_err()) return StreamFormatVerdict::Invalid;
        raw.extra = extra.value();
        if (raw.extra < WAVEFORMATEXTENSIBLE_EXTRA) return StreamFormatVerdict::Invalid;
        auto valid = mem.read_u16(addr + 18);
        auto mask = mem.read_u32(addr + 20);
        auto subformat = m.read_struct<Guid>(addr + 24);
        if (valid.is_err() || mask.is_err() || subformat.is_err()) {
            return StreamFormatVerdict::Invalid;
        }
        // wValidBitsPerSample shares a union with wSamplesPerBlock; 0 means
        // the whole container is significant.
        if (valid.value() != 0) raw.valid_bits = valid.value();
        raw.channel_mask = mask.value();
        raw.subformat = subformat.value();
        if (guid_equal(raw.subformat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            is_float = true;
        } else if (!guid_equal(raw.subformat, KSDATAFORMAT_SUBTYPE_PCM)) {
            if (raw.channels == 0 || raw.rate == 0) return StreamFormatVerdict::Invalid;
            return StreamFormatVerdict::Unsupported;
        }
    } else if (raw.tag == WAVE_FORMAT_IEEE_FLOAT) {
        is_float = true;
    } else if (raw.tag != WAVE_FORMAT_PCM) {
        if (raw.channels == 0 || raw.rate == 0) return StreamFormatVerdict::Invalid;
        return StreamFormatVerdict::Unsupported;
    }
    // A linear format's frame is exactly nChannels containers.
    if (raw.channels == 0 || raw.rate == 0 || raw.bits == 0 || raw.bits % 8u != 0 ||
        raw.valid_bits > raw.bits ||
        raw.block_align != raw.channels * (raw.bits / 8u)) {
        return StreamFormatVerdict::Invalid;
    }
    out = audio::PcmFormat{raw.rate, raw.channels, raw.bits, is_float,
                           raw.channel_mask};
    audio::PcmFormat rate_checked = out;
    if (rate_checked.sample_rate >= min_rate && rate_checked.sample_rate < 100) {
        rate_checked.sample_rate = 100;  // the capture floor is below the voice's
    }
    return rate_checked.valid_stream(max_channels) ? StreamFormatVerdict::Supported
                                                   : StreamFormatVerdict::Unsupported;
}

/// Shared mode without AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM: Learn
/// (IsFormatSupported) — the engine supports the mix format and "similar
/// formats that have the same sample rate and number of channels as the mix
/// format but differ in the representation of audio sample values".
[[nodiscard]] inline bool matches_mix_rate_and_channels(const audio::PcmFormat& format) {
    return format.sample_rate == MixWaveFormat::SAMPLES_PER_SEC &&
           format.channels == MixWaveFormat::CHANNELS;
}

}  // namespace

}  // namespace vitreo::winapi
