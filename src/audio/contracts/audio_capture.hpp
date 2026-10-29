#pragma once
/// @file audio_capture.hpp
/// @brief Frontend-neutral audio input (microphone) contract.
///
/// The capture twin of audio_renderer.hpp. It describes what the Windows
/// capture frontends (DirectSound capture, waveIn, WASAPI capture) need from
/// the host and contains no API-specific or platform-specific types:
///   - the host input devices, and which one is the default;
///   - the microphone permission state (macOS TCC);
///   - a capture stream that delivers frames in the format the frontend asks
///     for into a CaptureRing, stamped with a device position, a QPC time and
///     a discontinuity flag.
/// Backends live under src/audio/<backend>/.

#include "audio/contracts/audio_renderer.hpp"

#include <vitreo/common.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::audio {

/// Capture streams carry the input device's own channels; this bounds a
/// stream format (WAVEFORMATEXTENSIBLE has no smaller limit).
inline constexpr u16 kCaptureMaxChannels = 64;

/// One host input device.
struct CaptureDevice {
    /// Host-stable identity (CoreAudio kAudioDevicePropertyDeviceUID).
    std::string uid;
    /// Friendly name, UTF-16 as the Windows APIs report it.
    std::u16string name;
    /// The device's nominal sample rate and input channel count.
    u32 sample_rate{48000};
    u16 channels{2};
    /// WAVEFORMATEXTENSIBLE dwChannelMask of the device's channels in order
    /// (SPEAKER_* bits, ksmedia.h); 0 is KSAUDIO_SPEAKER_DIRECTOUT: channels
    /// that are not speaker positions.
    u32 channel_mask{0};
};

/// The speaker configuration of an input whose host layout names no
/// positions: KSAUDIO_SPEAKER_MONO, KSAUDIO_SPEAKER_STEREO, otherwise
/// KSAUDIO_SPEAKER_DIRECTOUT (ksmedia.h).
[[nodiscard]] constexpr u32 unlabelled_capture_channel_mask(u16 channels) noexcept {
    return channels == 1 ? 0x4u : channels == 2 ? 0x3u : 0x0u;
}

/// Whether a stream of `stream_channels` can be made exactly from a device
/// that delivers `device_channels`: the same count, mono copied to both
/// stereo channels, or stereo averaged to mono. Any other mix needs a
/// speaker-matrix decision the host does not make (frontends refuse it).
[[nodiscard]] constexpr bool capture_channels_convertible(u16 device_channels,
                                                          u16 stream_channels) noexcept {
    return device_channels == stream_channels ||
           (device_channels == 1 && stream_channels == 2) ||
           (device_channels == 2 && stream_channels == 1);
}

/// Convert `frames` frames of `format` samples from `device_channels`
/// channels to format.channels (see capture_channels_convertible): mono to
/// stereo copies the sample into both channels; stereo to mono is the mean
/// of the two, rounded toward negative infinity for integer samples.
inline void convert_capture_channels(const PcmFormat& format, u16 device_channels,
                                     const u8* source, u32 frames, u8* destination) {
    const usize bytes = format.bits_per_sample / 8u;
    if (device_channels == format.channels) {
        std::memcpy(destination, source, static_cast<usize>(frames) * device_channels * bytes);
        return;
    }
    auto load = [&](const u8* at) -> double {
        if (format.float_samples) {
            float f = 0.0f;
            std::memcpy(&f, at, sizeof(f));
            return f;
        }
        switch (bytes) {
        case 1: return static_cast<double>(at[0]) - 128.0;
        case 2: {
            i16 v = 0;
            std::memcpy(&v, at, sizeof(v));
            return v;
        }
        case 3: {
            const u32 raw = static_cast<u32>(at[0]) | (static_cast<u32>(at[1]) << 8) |
                            (static_cast<u32>(at[2]) << 16);
            return static_cast<i32>(raw << 8) >> 8;
        }
        default: {
            i32 v = 0;
            std::memcpy(&v, at, sizeof(v));
            return v;
        }
        }
    };
    auto store = [&](u8* at, double v) {
        if (format.float_samples) {
            const float f = static_cast<float>(v);
            std::memcpy(at, &f, sizeof(f));
            return;
        }
        const i64 n = static_cast<i64>(std::floor(v));
        switch (bytes) {
        case 1: at[0] = static_cast<u8>(n + 128); break;
        case 2: {
            const i16 s = static_cast<i16>(n);
            std::memcpy(at, &s, sizeof(s));
            break;
        }
        case 3:
            at[0] = static_cast<u8>(n);
            at[1] = static_cast<u8>(n >> 8);
            at[2] = static_cast<u8>(n >> 16);
            break;
        default: {
            const i32 s = static_cast<i32>(n);
            std::memcpy(at, &s, sizeof(s));
            break;
        }
        }
    };
    for (u32 frame = 0; frame < frames; ++frame) {
        const u8* in = source + static_cast<usize>(frame) * device_channels * bytes;
        u8* out = destination + static_cast<usize>(frame) * format.channels * bytes;
        if (device_channels == 1) {
            std::memcpy(out, in, bytes);
            std::memcpy(out + bytes, in, bytes);
        } else {
            store(out, (load(in) + load(in + bytes)) / 2.0);
        }
    }
}

/// The shared-mode capture engine's packetization (measured on Windows:
/// audio_capture_contract_probe "wasapi src", "wasapi positions"). The engine
/// runs in 10 ms periods k = 1, 2, ...; the packet of period k sits at device
/// position k * period_frames (engine frames) and holds the stream frames
/// [end(k - 1), end(k)), where end(k) = max(0, ceil(k * rate / 100 - delay))
/// and `delay` is the stream converter's delay in stream frames (0 without a
/// converter).
struct CapturePeriods {
    u32 rate{48000};
    u64 delay_numerator{0};
    u64 delay_denominator{1};
    u32 period_frames{480};

    /// Stream frames produced by the end of period k.
    [[nodiscard]] u64 end(u64 k) const noexcept {
        using wide = unsigned __int128;
        const wide whole = static_cast<wide>(k) * rate * delay_denominator;
        const wide delay = static_cast<wide>(delay_numerator) * 100u;
        if (whole <= delay) return 0;
        const wide denominator = static_cast<wide>(delay_denominator) * 100u;
        return static_cast<u64>((whole - delay + denominator - 1) / denominator);
    }
    /// The period that delivers stream frame `frame`: end(k - 1) <= frame < end(k).
    [[nodiscard]] u64 period_of(u64 frame) const noexcept {
        using wide = unsigned __int128;
        const wide numerator =
            (static_cast<wide>(frame) * delay_denominator + delay_numerator) * 100u;
        return static_cast<u64>(numerator / (static_cast<wide>(rate) * delay_denominator)) + 1;
    }
    /// `frame` when a period starts there, else the start of the next period.
    [[nodiscard]] u64 boundary_at_or_after(u64 frame) const noexcept {
        const u64 k = period_of(frame);
        return end(k - 1) == frame ? frame : end(k);
    }
    /// Device position (engine frames) of period k's packet.
    [[nodiscard]] u64 position(u64 k) const noexcept { return k * period_frames; }
};

/// Microphone permission. Undetermined means the user has not been asked yet.
enum class CapturePermission : u8 { Granted, Denied, Undetermined };

/// FIFO of captured frames between a backend (producer) and one frontend
/// (consumer), with the bookkeeping every Windows capture API reports.
///
/// - Every produced frame has a stream position: the number of frames the
///   stream delivered before it since the ring was created (or cleared with
///   reset_position), counting frames that were lost. A frontend with
///   engine periods (set_periods) maps it to the period's device position.
/// - When the ring is full the NEWEST frames are dropped, as the Windows
///   shared-mode engine does (audio_capture_contract_probe "wasapi overrun":
///   the buffer keeps the oldest frames, contiguous, and both the packet that
///   ends at the gap and the first packet after it carry
///   DATA_DISCONTINUITY).
/// - The first frame after a start (or a clear) starts a discontinuous span.
/// Thread-safe: the producer and the consumer may run on different threads.
class CaptureRing {
public:
    /// A run of frames that is contiguous in device position.
    struct Span {
        u64 position{0};
        u64 qpc_hns{0};
        bool discontinuity{false};
        u32 frames{0};
    };

    CaptureRing(u32 frame_bytes, u32 capacity_frames, u32 sample_rate)
        : m_frame_bytes(frame_bytes), m_capacity(capacity_frames),
          m_rate(sample_rate == 0 ? 1 : sample_rate),
          m_data(static_cast<usize>(frame_bytes) * capacity_frames) {}

    [[nodiscard]] u32 frame_bytes() const noexcept { return m_frame_bytes; }
    [[nodiscard]] u32 capacity() const noexcept { return m_capacity; }
    [[nodiscard]] u32 sample_rate() const noexcept { return m_rate; }

    /// Deliver in engine periods (WASAPI): after frames are lost the ring
    /// resumes at the next period boundary, and so does a restart.
    void set_periods(const CapturePeriods& periods) {
        std::lock_guard lock(m_mutex);
        m_periods = periods;
        m_has_periods = true;
    }

    /// Producer: append `count` frames captured at `qpc_hns` (100 ns units of
    /// the guest QPC clock). Frames that do not fit are dropped.
    void push(const u8* frames, u32 count, u64 qpc_hns) {
        std::lock_guard lock(m_mutex);
        if (m_has_periods && m_resync && count != 0) {
            // The rest of the period the loss began in is lost too.
            const u64 boundary = m_periods.boundary_at_or_after(m_next_position);
            const u32 skip = static_cast<u32>(std::min<u64>(count, boundary - m_next_position));
            frames += static_cast<usize>(skip) * m_frame_bytes;
            count -= skip;
            m_next_position += skip;
            qpc_hns += static_cast<u64>(skip) * 10'000'000ull / m_rate;
            if (m_next_position == boundary) m_resync = false;
            if (count == 0) return;
        }
        const u32 space = m_capacity - m_count;
        const u32 take = std::min(count, space);
        if (take != 0) {
            const bool new_span = m_spans.empty() || m_pending_discontinuity ||
                m_spans.back().position + m_spans.back().frames != m_next_position;
            if (new_span) {
                m_spans.push_back(StoredSpan{m_next_position, qpc_hns,
                                             m_pending_discontinuity, false, take});
            } else {
                m_spans.back().frames += take;
            }
            const usize bytes = m_frame_bytes;
            u32 write = (m_read + m_count) % m_capacity;
            for (u32 copied = 0; copied < take;) {
                const u32 run = std::min(take - copied, m_capacity - write);
                std::memcpy(m_data.data() + static_cast<usize>(write) * bytes,
                            frames + static_cast<usize>(copied) * bytes,
                            static_cast<usize>(run) * bytes);
                copied += run;
                write = (write + run) % m_capacity;
            }
            m_count += take;
            m_stored_total += take;
            m_pending_discontinuity = false;
        }
        if (take < count) {
            // The span that ends at the gap is flagged, and so is the next.
            if (!m_spans.empty()) m_spans.back().gap_after = true;
            m_pending_discontinuity = true;
            m_resync = true;
        }
        m_next_position += count;
    }

    /// Frames waiting to be read.
    [[nodiscard]] u32 available() const {
        std::lock_guard lock(m_mutex);
        return m_count;
    }

    /// The next contiguous run of at most `max_frames` frames.
    [[nodiscard]] Span next(u32 max_frames) const {
        std::lock_guard lock(m_mutex);
        if (m_spans.empty() || max_frames == 0) return {};
        const StoredSpan& s = m_spans.front();
        Span out;
        out.position = s.position;
        out.qpc_hns = s.qpc_hns;
        out.frames = std::min(s.frames, max_frames);
        out.discontinuity = s.discontinuity || (s.gap_after && out.frames == s.frames);
        return out;
    }

    /// True when another stored run follows the front one (the front run is
    /// complete: no later frame can extend it).
    [[nodiscard]] bool front_span_closed() const {
        std::lock_guard lock(m_mutex);
        return m_spans.size() > 1 || (!m_spans.empty() && m_spans.front().gap_after);
    }

    /// Copy `frames` frames from the front into `destination` without
    /// consuming them. Returns the number copied.
    u32 peek(u8* destination, u32 frames) const {
        std::lock_guard lock(m_mutex);
        return copy_locked(destination, std::min(frames, m_count));
    }

    /// Consume `frames` frames from the front.
    void consume(u32 frames) {
        std::lock_guard lock(m_mutex);
        frames = std::min(frames, m_count);
        m_read = (m_read + frames) % std::max<u32>(m_capacity, 1);
        m_count -= frames;
        while (frames != 0 && !m_spans.empty()) {
            StoredSpan& s = m_spans.front();
            const u32 take = std::min(frames, s.frames);
            s.frames -= take;
            s.position += take;
            s.qpc_hns += static_cast<u64>(take) * 10'000'000ull / m_rate;
            s.discontinuity = false;
            frames -= take;
            if (s.frames == 0) m_spans.pop_front();
        }
    }

    /// Copy and consume up to `frames` frames. Returns the number read.
    u32 read(u8* destination, u32 frames) {
        const u32 copied = peek(destination, frames);
        consume(copied);
        return copied;
    }

    /// Drop every buffered frame. `reset_position` also restarts the device
    /// position at 0 (IAudioClient::Reset). The next frame is discontinuous.
    void clear(bool reset_position) {
        std::lock_guard lock(m_mutex);
        m_read = 0;
        m_count = 0;
        m_spans.clear();
        m_pending_discontinuity = true;
        if (reset_position) {
            m_next_position = 0;
            m_stored_total = 0;
        }
    }

    /// The next stored frame starts a discontinuous span (a stream start).
    /// With periods, the stream resumes at the next period boundary (measured:
    /// a restart's first packet is the period after the one Stop ended in).
    void mark_discontinuity() {
        std::lock_guard lock(m_mutex);
        m_pending_discontinuity = true;
        if (m_has_periods) m_next_position = m_periods.boundary_at_or_after(m_next_position);
        m_resync = false;
    }

    /// Restart the count of stored frames (IAudioClient::Reset zeroes the
    /// clock) without moving the device position.
    void reset_stored_total() {
        std::lock_guard lock(m_mutex);
        m_stored_total = 0;
    }

    /// Frames ever stored (not dropped) since creation or the last reset.
    [[nodiscard]] u64 stored_total() const {
        std::lock_guard lock(m_mutex);
        return m_stored_total;
    }

    /// Device position of the next frame the producer delivers.
    [[nodiscard]] u64 next_position() const {
        std::lock_guard lock(m_mutex);
        return m_next_position;
    }

private:
    struct StoredSpan {
        u64 position;
        u64 qpc_hns;
        bool discontinuity;
        bool gap_after;
        u32 frames;
    };

    u32 copy_locked(u8* destination, u32 frames) const {
        const usize bytes = m_frame_bytes;
        u32 read = m_read;
        for (u32 copied = 0; copied < frames;) {
            const u32 run = std::min(frames - copied, m_capacity - read);
            std::memcpy(destination + static_cast<usize>(copied) * bytes,
                        m_data.data() + static_cast<usize>(read) * bytes,
                        static_cast<usize>(run) * bytes);
            copied += run;
            read = (read + run) % m_capacity;
        }
        return frames;
    }

    const u32 m_frame_bytes;
    const u32 m_capacity;
    const u32 m_rate;
    mutable std::mutex m_mutex;
    std::vector<u8> m_data;
    u32 m_read{0};
    u32 m_count{0};
    std::deque<StoredSpan> m_spans;
    u64 m_next_position{0};
    u64 m_stored_total{0};
    bool m_pending_discontinuity{true};
    CapturePeriods m_periods{};
    bool m_has_periods{false};
    bool m_resync{false};
};

/// A capture stream: the backend fills ring() while running and calls the
/// stream's data callback after each delivery (from a backend thread, which
/// must never run guest code: frontends only copy, signal events and queue
/// completions there). start(), stop() and destruction may wait for an
/// in-flight data callback, so a frontend must not call them while holding a
/// lock that its data callback takes.
class IAudioCaptureStream {
public:
    virtual ~IAudioCaptureStream() = default;
    [[nodiscard]] virtual bool start() = 0;
    virtual void stop() = 0;
    /// Stop delivering without waiting: the one stop a data callback may
    /// request (a non-looping DirectSound capture that filled its buffer).
    /// A later start() restarts the stream.
    virtual void request_stop() = 0;
    [[nodiscard]] virtual CaptureRing& ring() = 0;
};

/// Device-level capture factory.
class IAudioCapture {
public:
    virtual ~IAudioCapture() = default;

    /// The host input devices, in host order. Empty when there are none.
    [[nodiscard]] virtual std::vector<CaptureDevice> devices() = 0;
    /// UID of the default input device, or empty when there is none.
    [[nodiscard]] virtual std::string default_device() = 0;

    /// The current microphone permission, without prompting.
    [[nodiscard]] virtual CapturePermission permission() = 0;
    /// Ask the user when the permission is undetermined and wait for the
    /// answer. Blocks the calling (guest) thread; never prompts from the
    /// host main thread.
    [[nodiscard]] virtual CapturePermission request_permission() = 0;
    /// A description of whom the host judges for the permission (diagnostic
    /// text for the first capture open; empty when there is nothing to add).
    [[nodiscard]] virtual std::string permission_context() { return {}; }

    /// Open a stream on device `uid` that delivers `format` frames into a ring
    /// of `ring_frames` frames. Returns null when the device is gone or the
    /// format cannot be produced.
    [[nodiscard]] virtual std::unique_ptr<IAudioCaptureStream> open(
        const std::string& uid, const PcmFormat& format, u32 ring_frames,
        std::function<void()> on_data) = 0;
};

/// Create the platform capture backend (never null; it may list no devices).
[[nodiscard]] std::unique_ptr<IAudioCapture> create_audio_capture();

/// The permission a frontend acts on when a stream opens: asks once when the
/// user has not decided yet. Every refusal is logged once per process
/// ([vitreo] line), because the Windows result for a denied microphone is not
/// measured (.docs/contracts: "denied consent").
[[nodiscard]] inline bool capture_permitted(IAudioCapture& capture, const char* api) {
    const CapturePermission before = capture.permission();
    CapturePermission state = before;
    if (state == CapturePermission::Undetermined) state = capture.request_permission();
    // One line per process, at the first capture open: what macOS reported,
    // what Vitreo did about it, and whom the host judged (backend context).
    static std::once_flag once;
    std::call_once(once, [&] {
        const char* status = before == CapturePermission::Granted ? "authorized"
            : before == CapturePermission::Denied              ? "denied or restricted"
                                                               : "not determined";
        const char* decision = before == CapturePermission::Granted ? "capturing"
            : before == CapturePermission::Denied
                ? "refused without asking (an earlier decision or policy)"
            : state == CapturePermission::Granted ? "asked the user: allowed"
                                                  : "asked the user (or could not ask): refused";
        const std::string context = capture.permission_context();
        std::fprintf(stderr, "[vitreo] audio capture: %s: microphone authorization %s; %s%s%s%s\n",
                     api, status, decision, context.empty() ? "" : "; ", context.c_str(),
                     state == CapturePermission::Granted
                         ? ""
                         : "; the call fails with its documented access-denied result "
                           "(Windows value unmeasured)");
    });
    return state == CapturePermission::Granted;
}

/// The capture devices in Windows device-id order: the default device first,
/// then the others in host order. DirectSound enumeration (after the primary
/// driver entry) and waveIn device ids use this order; IMMDeviceCollection
/// uses host order (audio_capture_contract_probe: "default-first-then-mm-order").
[[nodiscard]] inline std::vector<CaptureDevice> capture_devices_in_id_order(
    IAudioCapture& capture) {
    std::vector<CaptureDevice> devices = capture.devices();
    const std::string def = capture.default_device();
    auto it = std::find_if(devices.begin(), devices.end(),
                           [&](const CaptureDevice& d) { return d.uid == def; });
    if (it != devices.end() && it != devices.begin()) {
        CaptureDevice first = *it;
        devices.erase(it);
        devices.insert(devices.begin(), std::move(first));
    }
    return devices;
}

/// A stable GUID for a host device: the identity DirectSound enumerates and
/// the {guid} part of the MMDevice endpoint id (measured: the DirectSound
/// device GUID equals the endpoint id's GUID). Name-derived and deterministic,
/// with the RFC 4122 variant and version-5 bits set.
[[nodiscard]] inline std::array<u8, 16> capture_device_guid(std::string_view uid) {
    auto fnv = [&](u64 seed) {
        u64 h = seed;
        for (const char c : uid) {
            h ^= static_cast<u8>(c);
            h *= 0x100000001B3ull;
        }
        return h;
    };
    const u64 a = fnv(0xCBF29CE484222325ull ^ 0x56697472756D4361ull);  // "VitreoCa"
    const u64 b = fnv(0x84222325CBF29CE4ull ^ 0x7074757265446576ull);  // "ptureDev"
    std::array<u8, 16> g{};
    for (usize i = 0; i < 8; ++i) {
        g[i] = static_cast<u8>(a >> (8u * i));
        g[8u + i] = static_cast<u8>(b >> (8u * i));
    }
    g[7] = static_cast<u8>((g[7] & 0x0F) | 0x50);  // Data3 high nibble: version 5
    g[8] = static_cast<u8>((g[8] & 0x3F) | 0x80);  // variant 10xx
    return g;
}

/// The MMDevice endpoint id of a capture device: "{0.0.1.00000000}.{guid}",
/// the Windows capture-endpoint form (flow 1), GUID in lower case.
[[nodiscard]] inline std::string capture_endpoint_id(std::string_view uid) {
    const std::array<u8, 16> g = capture_device_guid(uid);
    const u32 d1 = static_cast<u32>(g[0]) | (static_cast<u32>(g[1]) << 8) |
                   (static_cast<u32>(g[2]) << 16) | (static_cast<u32>(g[3]) << 24);
    const u32 d2 = static_cast<u32>(g[4]) | (static_cast<u32>(g[5]) << 8);
    const u32 d3 = static_cast<u32>(g[6]) | (static_cast<u32>(g[7]) << 8);
    char text[64];
    std::snprintf(text, sizeof(text),
                  "{0.0.1.00000000}.{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                  d1, d2, d3, g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return text;
}

}  // namespace vitreo::audio
