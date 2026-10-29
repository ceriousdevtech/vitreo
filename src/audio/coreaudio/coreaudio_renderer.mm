/// @file coreaudio_renderer.mm
/// @brief macOS AudioQueue implementation of the streaming PCM contract.

#include "audio/contracts/audio_renderer.hpp"

#include <vitreo/present_cpu.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>

namespace vitreo::audio {
namespace {

/// Number of queue buffers kept in flight. Three gives the device a stable
/// refill window without adding audible latency.
constexpr u32 QUEUE_BUFFERS = 3;

/// Target duration of one queue buffer. Guest code polls the play cursor at
/// frame rate, so chunks must stay well below a frame.
constexpr double CHUNK_SECONDS = 0.020;

/// Convert hundredths of a decibel of attenuation to a linear gain.
float gain_from_hundredth_db(i32 hundredth_db) {
    if (hundredth_db <= -10000) return 0.0f;
    if (hundredth_db >= 0) return 1.0f;
    return static_cast<float>(std::pow(10.0, static_cast<double>(hundredth_db) / 2000.0));
}

class CoreAudioVoice final : public IAudioVoice {
public:
    CoreAudioVoice(const PcmFormat& format, const u8* ring, u32 ring_bytes)
        : m_format(format), m_ring(ring), m_ring_bytes(ring_bytes),
          m_playback_rate(format.sample_rate) {}

    ~CoreAudioVoice() override {
        teardown_queue();
    }

    [[nodiscard]] bool initialize() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return build_queue();
    }

    [[nodiscard]] bool play(bool looping) override {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_queue && !build_queue()) return false;
        const bool was_looping = m_looping;
        m_looping = looping;
        if (m_playing) {
            // DirectSound permits Play on an already-playing buffer and the
            // newest flags replace the previous flags.  When looping is
            // removed, drain only to the next end of the ring.
            if (was_looping && !m_looping) {
                m_remaining = m_ring_bytes - m_read_offset;
            }
            return true;
        }
        if (m_paused) {
            m_playing = true;
            m_paused = false;
            m_have_start_time = false;
            if (AudioQueueStart(m_queue, nullptr) != noErr) {
                m_playing = false;
                m_paused = true;
                return false;
            }
            capture_start_time_locked();
            return true;
        }

        // A prior Stop drains the committed write-ahead without Reset. Flush
        // that queue before re-priming so the new Play starts at the frozen
        // cursor instead of mixing with the leftover chunks.
        if (m_flush_before_play && m_queue) {
            AudioQueueRef queue = m_queue;
            m_flush_before_play = false;
            lock.unlock();
            (void)AudioQueueStop(queue, true);
            (void)AudioQueueReset(queue);
            lock.lock();
            if (m_queue != queue) return false;
        }

        // A naturally completed buffer leaves its circular cursor at zero.
        // An explicitly stopped buffer resumes at m_position.
        if (!m_looping && m_remaining == 0) {
            m_read_offset = m_position;
            m_remaining = m_ring_bytes - m_position;
        }
        return prime_and_start_locked();
    }

    void pause() override {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_queue || !m_playing) return;
        AudioQueueRef queue = m_queue;
        lock.unlock();
        const OSStatus result = AudioQueuePause(queue);
        lock.lock();
        if (m_queue != queue || result != noErr) return;
        m_position = cursor_locked();
        m_playing = false;
        m_paused = true;
        m_have_start_time = false;
    }

    void stop() override {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_queue || (!m_playing && !m_paused)) return;
        if (m_playing) m_position = cursor_locked();
        // Rewind the feed to the audible cursor. Bytes already handed to the
        // device but not yet played must be re-fed, or a resumed buffer skips
        // up to one full queue's worth of audio.
        m_read_offset = m_position;
        if (!m_looping && m_ring_bytes != 0) {
            m_remaining = m_ring_bytes - m_position;
        }
        m_playing = false;
        m_paused = false;
        m_draining = false;
        m_flush_before_play = true;
        AudioQueueRef queue = m_queue;
        // AudioQueueStop(synchronous) waits for the callback thread, which
        // takes m_mutex. Release it first or the two deadlock.
        lock.unlock();
        // Play already committed the write-ahead (the play..write span).
        // Immediate stop+reset discards that region, so a static one-shot
        // that Play+Stops in one timeslice is silent. Drain queued buffers
        // instead; GetStatus is already not PLAYING.
        // https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee418744(v=vs.85)
        (void)AudioQueueStop(queue, false);
    }

    [[nodiscard]] bool is_playing() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_playing;
    }

    [[nodiscard]] u32 play_cursor() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_playing ? cursor_locked() : m_position;
    }

    [[nodiscard]] u32 write_cursor() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_ring_bytes == 0) return 0;
        if (!m_playing) return m_position;
        // DirectSound's write cursor is the first forward byte that can be
        // replaced without racing the device.  CoreAudio has already copied
        // everything up to m_read_offset into its native queue, so reporting
        // only one chunk ahead of the audible cursor lets streaming clients
        // rewrite data that is already queued and produces stale short loops.
        return m_read_offset;
    }

    void set_position(u32 byte_offset) override {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_ring_bytes == 0) return;
        const u32 position = align_to_frame(byte_offset % m_ring_bytes);
        const bool was_playing = m_playing;
        const bool was_paused = m_paused;
        AudioQueueRef queue = m_queue;
        if ((was_playing || was_paused) && queue != nullptr) {
            // SetCurrentPosition takes effect immediately. Discard native
            // buffers copied from the old play position before re-priming.
            m_playing = false;
            m_paused = false;
            m_draining = false;
            lock.unlock();
            (void)AudioQueueStop(queue, true);
            (void)AudioQueueReset(queue);
            lock.lock();
            if (m_queue != queue) return;
        }
        m_position = position;
        m_read_offset = m_position;
        m_have_start_time = false;
        m_src_phase = 0.0;
        if (!m_looping) m_remaining = m_ring_bytes - m_position;
        if (was_playing) (void)prime_and_start_locked();
    }

    void set_volume(i32 hundredth_db) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_volume = hundredth_db;
        if (m_queue) {
            (void)AudioQueueSetParameter(m_queue, kAudioQueueParam_Volume,
                                         gain_from_hundredth_db(hundredth_db));
        }
    }

    void set_pan(i32 hundredth_db) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pan = std::clamp(hundredth_db, -10000, 10000);
        if (m_queue) {
            (void)AudioQueueSetParameter(m_queue, kAudioQueueParam_Pan,
                                         static_cast<float>(m_pan) / 10000.0f);
        }
    }

    void set_frequency(u32 sample_rate) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        // 0 means "restore the format's own rate" in DirectSound.
        const u32 requested =
            sample_rate == 0 ? m_format.sample_rate : sample_rate;
        if (requested == m_playback_rate) return;
        m_playback_rate = requested;
        m_src_phase = 0.0;
        // Keep the AudioQueue at the buffer's native sample rate. Pitch /
        // speed changes are applied by resampling in fill_buffer_locked.
        // Rebuilding the queue on every SetFrequency (RCT3 park SFX / rides
        // spam this) dominated host CPU — AudioQueueDispose + NewOutput +
        // Start on the UI thread.
    }

private:
    /// Round a byte offset down to a frame boundary. Reading PCM from a
    /// mid-frame offset shifts every sample and turns the stream into noise,
    /// so every cursor this class produces passes through here.
    [[nodiscard]] u32 align_to_frame(u32 offset) const {
        const u32 align = m_format.block_align();
        return align == 0 ? offset : offset - (offset % align);
    }

    [[nodiscard]] u32 chunk_bytes_locked() const {
        const u32 align = m_format.block_align();
        if (align == 0) return 0;
        // Chunk size follows the AudioQueue's native rate (format), not the
        // DirectSound playback frequency — the queue is never rebuilt for
        // SetFrequency.
        const auto frames = static_cast<u32>(
            static_cast<double>(m_format.sample_rate) * CHUNK_SECONDS);
        return std::max(align, frames * align);
    }

    /// Byte offset the device has actually rendered, derived from the queue's
    /// own sample clock rather than from how much has been enqueued.
    [[nodiscard]] u32 cursor_locked() const {
        if (m_ring_bytes == 0) return 0;
        const u32 align = m_format.block_align();
        if (align == 0 || !m_queue) return m_position;

        AudioTimeStamp now{};
        Boolean discontinuity = false;
        if (AudioQueueGetCurrentTime(m_queue, nullptr, &now, &discontinuity) != noErr ||
            (now.mFlags & kAudioTimeStampSampleTimeValid) == 0) {
            return m_position;
        }
        if (!m_have_start_time) {
            m_start_sample_time = now.mSampleTime;
            m_have_start_time = true;
        }
        const double frames = now.mSampleTime - m_start_sample_time;
        if (frames <= 0) return m_position;

        // Output runs at format.sample_rate; source consumption scales with
        // DirectSound SetFrequency via the fill-time resampler.
        const double rate = static_cast<double>(
            m_playback_rate == 0 ? m_format.sample_rate : m_playback_rate);
        const double native = static_cast<double>(
            m_format.sample_rate == 0 ? 1u : m_format.sample_rate);
        const double source_frames = frames * (rate / native);
        const auto played = static_cast<u64>(source_frames) * align;
        if (!m_looping) {
            const u64 end = static_cast<u64>(m_position) + played;
            return end >= m_ring_bytes ? align_to_frame(m_ring_bytes - align)
                                       : align_to_frame(static_cast<u32>(end));
        }
        return align_to_frame(
            static_cast<u32>((m_position + played) % m_ring_bytes));
    }

    void capture_start_time_locked() {
        AudioTimeStamp now{};
        Boolean discontinuity = false;
        if (m_queue != nullptr &&
            AudioQueueGetCurrentTime(m_queue, nullptr, &now, &discontinuity) == noErr &&
            (now.mFlags & kAudioTimeStampSampleTimeValid) != 0) {
            m_start_sample_time = now.mSampleTime;
            m_have_start_time = true;
        }
    }

    [[nodiscard]] bool prime_and_start_locked() {
        m_draining = false;
        m_have_start_time = false;
        m_src_phase = 0.0;
        for (u32 i = 0; i < QUEUE_BUFFERS; ++i) {
            if (!fill_buffer_locked(m_buffers[i])) break;
        }
        // Set this before AudioQueueStart. The output callback can run as soon
        // as Start hands the queue to CoreAudio.
        m_playing = true;
        if (AudioQueueStart(m_queue, nullptr) != noErr) {
            VITREO_TRACE("[audio] AudioQueueStart failed\n");
            m_playing = false;
            return false;
        }
        capture_start_time_locked();
        if (!m_looping && m_remaining == 0) {
            schedule_drain_locked(m_queue);
        }
        return true;
    }

    void schedule_drain_locked(AudioQueueRef queue) {
        if (m_looping || m_draining || queue == nullptr) return;
        m_draining = true;
        // A non-immediate stop consumes every buffer already queued. Marking
        // the DirectSound voice idle here would be premature; the running
        // property listener does that only after CoreAudio finishes draining.
        (void)AudioQueueStop(queue, false);
    }

    /// Copy the next chunk out of the guest ring into a queue buffer.
    /// @return false when the voice has no more data to deliver
    bool fill_buffer_locked(AudioQueueBufferRef buffer) {
        if (!m_ring || m_ring_bytes == 0) return false;
        const u32 capacity = buffer->mAudioDataBytesCapacity;
        const u32 frame_bytes = m_format.block_align();
        if (frame_bytes == 0) return false;

        // Fast path: DirectSound frequency matches the buffer format — 1:1 copy.
        if (m_playback_rate == 0 || m_playback_rate == m_format.sample_rate) {
            u32 written = 0;
            while (written < capacity) {
                u32 available = m_ring_bytes - m_read_offset;
                if (!m_looping) {
                    if (m_remaining == 0) break;
                    available = std::min(available, m_remaining);
                }
                const u32 copy = std::min(capacity - written, available);
                if (copy == 0) break;

                std::memcpy(static_cast<u8*>(buffer->mAudioData) + written,
                            m_ring + m_read_offset, copy);
                written += copy;
                m_read_offset = (m_read_offset + copy) % m_ring_bytes;
                if (!m_looping) m_remaining -= copy;
            }
            if (written == 0) return false;
            buffer->mAudioDataByteSize = written;
            static const std::optional<std::string> dump_path =
                []() -> std::optional<std::string> {
                const char* path = std::getenv("VITREO_COREAUDIO_DUMP");
                if (path == nullptr) return std::nullopt;
                return std::string(path);
            }();
            if (dump_path.has_value()) {
                static std::mutex dump_mutex;
                static FILE* dump = nullptr;
                std::lock_guard dump_lock(dump_mutex);
                if (dump == nullptr) dump = std::fopen(dump_path->c_str(), "ab");
                if (dump != nullptr) {
                    (void)std::fwrite(buffer->mAudioData, 1u, written, dump);
                    (void)std::fflush(dump);
                }
            }
            return AudioQueueEnqueueBuffer(m_queue, buffer, 0, nullptr) == noErr;
        }

        // Pitch/speed change without rebuilding AudioQueue: nearest-neighbour
        // resample from the ring into output that runs at format.sample_rate.
        const double ratio = static_cast<double>(m_playback_rate) /
                             static_cast<double>(m_format.sample_rate);
        const u32 out_frames = capacity / frame_bytes;
        if (out_frames == 0) return false;
        auto* dst = static_cast<u8*>(buffer->mAudioData);
        u32 written_frames = 0;
        for (; written_frames < out_frames; ++written_frames) {
            if (!m_looping && m_remaining < frame_bytes) break;
            const u32 src_off = align_to_frame(m_read_offset);
            std::memcpy(dst + written_frames * frame_bytes, m_ring + src_off,
                        frame_bytes);
            // Advance source by `ratio` frames (fractional accumulator).
            m_src_phase += ratio;
            const u32 advance_frames = static_cast<u32>(m_src_phase);
            if (advance_frames != 0) {
                m_src_phase -= static_cast<double>(advance_frames);
                const u32 advance_bytes = advance_frames * frame_bytes;
                if (!m_looping) {
                    if (advance_bytes >= m_remaining) {
                        m_read_offset =
                            (m_read_offset + m_remaining) % m_ring_bytes;
                        m_remaining = 0;
                        ++written_frames; // count this last frame
                        break;
                    }
                    m_remaining -= advance_bytes;
                }
                m_read_offset = (m_read_offset + advance_bytes) % m_ring_bytes;
            }
        }
        if (written_frames == 0) return false;
        buffer->mAudioDataByteSize = written_frames * frame_bytes;
        return AudioQueueEnqueueBuffer(m_queue, buffer, 0, nullptr) == noErr;
    }

    static void buffer_callback(void* user_data, AudioQueueRef queue,
                                AudioQueueBufferRef buffer) {
        auto* voice = static_cast<CoreAudioVoice*>(user_data);
        std::lock_guard<std::mutex> lock(voice->m_mutex);
        if (!voice->m_playing) return;
        if (!voice->fill_buffer_locked(buffer)) {
            voice->schedule_drain_locked(queue);
        }
    }

    static void running_property_callback(void* user_data, AudioQueueRef queue,
                                          AudioQueuePropertyID property) {
        if (property != kAudioQueueProperty_IsRunning) return;
        UInt32 running = 0;
        UInt32 size = sizeof(running);
        if (AudioQueueGetProperty(queue, property, &running, &size) != noErr ||
            running != 0) {
            return;
        }

        auto* voice = static_cast<CoreAudioVoice*>(user_data);
        std::lock_guard<std::mutex> lock(voice->m_mutex);
        if (!voice->m_playing || !voice->m_draining) return;
        voice->m_playing = false;
        voice->m_draining = false;
        // The DirectSound buffer is circular. Natural non-looping completion
        // crosses its end and leaves the next play position at the beginning.
        voice->m_position = 0;
        voice->m_read_offset = 0;
        voice->m_remaining = 0;
        voice->m_have_start_time = false;
    }

    bool build_queue() {
        AudioStreamBasicDescription desc{};
        // Always create at the buffer's native rate. DirectSound SetFrequency
        // is handled by resampling in fill_buffer_locked.
        desc.mSampleRate = static_cast<Float64>(m_format.sample_rate);
        desc.mFormatID = kAudioFormatLinearPCM;
        desc.mFormatFlags = kAudioFormatFlagIsPacked |
                            (m_format.bits_per_sample == 16 ? kAudioFormatFlagIsSignedInteger : 0);
        desc.mChannelsPerFrame = m_format.channels;
        desc.mBitsPerChannel = m_format.bits_per_sample;
        desc.mFramesPerPacket = 1;
        desc.mBytesPerFrame = m_format.block_align();
        desc.mBytesPerPacket = desc.mBytesPerFrame;

        if (AudioQueueNewOutput(&desc, buffer_callback, this, nullptr, nullptr, 0,
                                &m_queue) != noErr || !m_queue) {
            VITREO_TRACE("[audio] AudioQueueNewOutput failed rate=%u ch=%u bits=%u\n",
                         m_format.sample_rate, m_format.channels, m_format.bits_per_sample);
            m_queue = nullptr;
            return false;
        }
        if (AudioQueueAddPropertyListener(
                m_queue, kAudioQueueProperty_IsRunning,
                running_property_callback, this) != noErr) {
            VITREO_TRACE("[audio] AudioQueue running listener failed\n");
            dispose_queue(detach_queue_locked());
            return false;
        }

        const u32 chunk = chunk_bytes_locked();
        for (u32 i = 0; i < QUEUE_BUFFERS; ++i) {
            if (AudioQueueAllocateBuffer(m_queue, chunk, &m_buffers[i]) != noErr) {
                VITREO_TRACE("[audio] AudioQueueAllocateBuffer failed\n");
                // Never started, so no callback thread can be waiting on the
                // mutex and disposing here cannot deadlock.
                dispose_queue(detach_queue_locked());
                return false;
            }
        }
        (void)AudioQueueSetParameter(m_queue, kAudioQueueParam_Volume,
                                     gain_from_hundredth_db(m_volume));
        (void)AudioQueueSetParameter(m_queue, kAudioQueueParam_Pan,
                                     static_cast<float>(m_pan) / 10000.0f);
        return true;
    }

    /// Detach the queue from this voice. The caller disposes it after
    /// releasing the mutex, because a synchronous dispose waits for the
    /// callback thread and the callback needs the mutex.
    [[nodiscard]] AudioQueueRef detach_queue_locked() {
        AudioQueueRef queue = m_queue;
        m_queue = nullptr;
        m_buffers = {};
        m_playing = false;
        m_paused = false;
        m_draining = false;
        return queue;
    }

    static void dispose_queue(AudioQueueRef queue) {
        // Synchronous so no callback can run against freed state afterwards.
        if (queue) (void)AudioQueueDispose(queue, true);
    }

    void teardown_queue() {
        std::unique_lock<std::mutex> lock(m_mutex);
        AudioQueueRef queue = detach_queue_locked();
        lock.unlock();
        dispose_queue(queue);
    }

    mutable std::mutex m_mutex;
    PcmFormat m_format;
    const u8* m_ring{nullptr};
    u32 m_ring_bytes{0};

    AudioQueueRef m_queue{nullptr};
    std::array<AudioQueueBufferRef, QUEUE_BUFFERS> m_buffers{};

    bool m_playing{false};
    bool m_paused{false};
    bool m_looping{false};
    bool m_draining{false};
    bool m_flush_before_play{false};
    u32 m_position{0};      ///< play position as of the last start/stop
    u32 m_read_offset{0};   ///< next byte to hand to the device
    u32 m_remaining{0};     ///< bytes left to deliver for a non-looping voice
    u32 m_playback_rate{0};
    i32 m_volume{0};
    i32 m_pan{0};
    /// Fractional source-frame phase for SetFrequency resampling.
    double m_src_phase{0.0};

    mutable bool m_have_start_time{false};
    mutable Float64 m_start_sample_time{0};
};

/// Push-voice (WASAPI shared mode) host pipeline. Queue buffers are short so
/// the device's IO cycle always finds frames queued; there are enough of
/// them to hold the host latency target plus a late engine pass.
constexpr u32 PUSH_QUEUE_BUFFERS = 16;
constexpr double PUSH_CHUNK_SECONDS = 0.005;
/// Frames the engine keeps between its read position and the device: covers
/// one device IO cycle (512 frames, 10.7 ms at 48 kHz, by default) plus one
/// engine period of burstiness, so a client that keeps one period of lead
/// in the endpoint buffer never lets the device run dry.
constexpr double PUSH_HOST_TARGET_SECONDS = 0.030;

/// The default output device's own latency (device + safety offset + IO
/// buffer), converted to stream frames; 0 when CoreAudio cannot say.
u32 output_device_latency_frames(u32 stream_rate) {
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDefaultOutputDevice,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    AudioObjectID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                   &device) != noErr || device == kAudioObjectUnknown) {
        return 0;
    }
    const auto read_u32 = [device](AudioObjectPropertySelector selector,
                                   AudioObjectPropertyScope scope) -> u32 {
        AudioObjectPropertyAddress property{selector, scope, kAudioObjectPropertyElementMain};
        UInt32 value = 0;
        UInt32 bytes = sizeof(value);
        return AudioObjectGetPropertyData(device, &property, 0, nullptr, &bytes, &value) == noErr
            ? value : 0u;
    };
    const u64 device_frames =
        static_cast<u64>(read_u32(kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput)) +
        read_u32(kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput) +
        read_u32(kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);
    AudioObjectPropertyAddress rate_address{kAudioDevicePropertyNominalSampleRate,
                                            kAudioObjectPropertyScopeGlobal,
                                            kAudioObjectPropertyElementMain};
    Float64 device_rate = 0;
    size = sizeof(device_rate);
    if (AudioObjectGetPropertyData(device, &rate_address, 0, nullptr, &size, &device_rate) !=
            noErr || !(device_rate > 0)) {
        return static_cast<u32>(std::min<u64>(device_frames, UINT32_MAX));
    }
    return static_cast<u32>(std::ceil(static_cast<double>(device_frames) *
                                      static_cast<double>(stream_rate) / device_rate));
}

/// A WASAPI shared-mode stream: the endpoint buffer the client writes, and
/// the engine that reads it once per period.
///
/// Learn (IAudioClient::GetCurrentPadding, Initialize): padding is the
/// "valid, unread data" in the endpoint buffer, the frames the audio engine
/// has not yet read, and "the latency through the buffer is determined
/// solely by the separation between the client's write pointer and the
/// engine's read pointer". So frames leave the endpoint buffer (and the
/// padding) only when engine_pass() reads them -- one engine period's worth
/// per pass, a shortfall becoming silence -- into the host pipeline (host
/// ring, then AudioQueue buffers), which is stream latency, not padding.
/// The host pipeline holds a constant PUSH_HOST_TARGET_SECONDS, re-primed
/// only after the device actually runs dry, and a slow trim of one frame per
/// pass keeps it there against drift between the engine clock and the
/// device clock.
class CoreAudioPushVoice final : public IAudioPushVoice {
public:
    CoreAudioPushVoice(const PcmFormat& format, u32 capacity_frames, bool rate_adjust)
        : m_format(format), m_align(format.block_align()), m_capacity_frames(capacity_frames),
          m_endpoint(static_cast<usize>(capacity_frames) * format.block_align(), 0u),
          m_gains(format.channels, 1.0f), m_rate_adjust(rate_adjust) {}

    ~CoreAudioPushVoice() override {
        std::unique_lock lock(m_mutex);
        AudioQueueRef queue = m_queue;
        m_queue = nullptr;
        m_playing = false;
        lock.unlock();
        if (queue != nullptr) (void)AudioQueueDispose(queue, true);
    }

    [[nodiscard]] bool initialize() {
        std::lock_guard lock(m_mutex);
        AudioStreamBasicDescription desc{};
        desc.mSampleRate = static_cast<Float64>(m_format.sample_rate);
        desc.mFormatID = kAudioFormatLinearPCM;
        // WAVE conventions: float is IEEE-754, 8-bit integer is unsigned, and
        // every wider integer is signed little-endian, left-justified in its
        // container (so a 24-in-32 sample plays as a full 32-bit sample).
        desc.mFormatFlags = kAudioFormatFlagIsPacked |
            (m_format.float_samples ? kAudioFormatFlagIsFloat
             : m_format.bits_per_sample > 8 ? kAudioFormatFlagIsSignedInteger
                                            : 0);
        desc.mChannelsPerFrame = m_format.channels;
        desc.mBitsPerChannel = m_format.bits_per_sample;
        desc.mFramesPerPacket = 1;
        desc.mBytesPerFrame = m_align;
        desc.mBytesPerPacket = desc.mBytesPerFrame;
        if (AudioQueueNewOutput(&desc, callback, this, nullptr, nullptr, 0,
                                &m_queue) != noErr || m_queue == nullptr) return false;
        // Speaker positions: CoreAudio's channel bitmap uses the same bit
        // assignment as the WAVEFORMATEXTENSIBLE SPEAKER_* mask (bit 0 front
        // left, 1 front right, 2 centre, 3 LFE, 4/5 back left/right, ...),
        // so the guest's mask places multichannel streams directly.
        if (m_format.channels > 2 && m_format.channel_mask != 0) {
            AudioChannelLayout layout{};
            layout.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelBitmap;
            layout.mChannelBitmap = static_cast<AudioChannelBitmap>(m_format.channel_mask);
            (void)AudioQueueSetProperty(m_queue, kAudioQueueProperty_ChannelLayout,
                                        &layout, sizeof(layout));
        }
        const double rate = static_cast<double>(m_format.sample_rate);
        m_chunk_frames = std::max(static_cast<u32>(rate * PUSH_CHUNK_SECONDS), 1u);
        m_target_frames = std::max(static_cast<u32>(rate * PUSH_HOST_TARGET_SECONDS),
                                   2u * m_chunk_frames);
        m_host_capacity = m_target_frames + m_format.sample_rate / 10u;
        m_host.assign(static_cast<usize>(m_host_capacity) * m_align, 0u);
        for (u32 index = 0; index < PUSH_QUEUE_BUFFERS; ++index) {
            if (AudioQueueAllocateBuffer(m_queue, m_chunk_frames * m_align,
                                         &m_buffers[index]) != noErr) return false;
        }
        (void)AudioQueueSetParameter(m_queue, kAudioQueueParam_Volume, 1.0f);
        if (m_rate_adjust) {
            // Varispeed: the play rate changes speed and pitch together, which
            // is what playing the frames at a different sample rate means.
            // Both properties must be set before the queue first starts.
            const UInt32 enable = 1;
            const UInt32 algorithm = kAudioQueueTimePitchAlgorithm_Varispeed;
            if (AudioQueueSetProperty(m_queue, kAudioQueueProperty_EnableTimePitch,
                                      &enable, sizeof(enable)) != noErr ||
                AudioQueueSetProperty(m_queue, kAudioQueueProperty_TimePitchAlgorithm,
                                      &algorithm, sizeof(algorithm)) != noErr) {
                return false;
            }
        }
        // Constant for the voice's life: the host pipeline at its target plus
        // one queue chunk of fill granularity, and the device's own latency.
        m_latency_frames = m_target_frames + m_chunk_frames +
                           output_device_latency_frames(m_format.sample_rate);
        return true;
    }

    void set_channel_gains(const float* gains, u32 count) override {
        std::lock_guard lock(m_mutex);
        m_unity_gain = true;
        for (u32 channel = 0; channel < m_gains.size(); ++channel) {
            m_gains[channel] = channel < count ? gains[channel] : 1.0f;
            if (m_gains[channel] != 1.0f) m_unity_gain = false;
        }
    }

    [[nodiscard]] bool set_rate_ratio(double ratio) override {
        // kAudioQueueParam_PlayRate accepts 0.5 through 2.0.
        if (!m_rate_adjust || !std::isfinite(ratio) || ratio < 0.5 || ratio > 2.0) {
            return false;
        }
        std::lock_guard lock(m_mutex);
        if (m_queue == nullptr ||
            AudioQueueSetParameter(m_queue, kAudioQueueParam_PlayRate,
                                   static_cast<AudioQueueParameterValue>(ratio)) != noErr) {
            return false;
        }
        m_rate_ratio = ratio;
        return true;
    }

    [[nodiscard]] bool start() override {
        std::unique_lock lock(m_mutex);
        if (m_queue == nullptr) return false;
        m_playing = true;
        m_drift = {};
        // The device runs from Start (playing silence until the engine has
        // primed the pipeline). Starting it here keeps AudioQueueStart, whose
        // first call spins the device up for tens of ms, off the engine
        // thread.
        const bool should_start = !m_queue_running;
        if (should_start) {
            m_queue_running = true;
            ++m_stats.starts;
        }
        AudioQueueRef queue = m_queue;
        lock.unlock();
        if (should_start && AudioQueueStart(queue, nullptr) != noErr) {
            lock.lock();
            m_queue_running = false;
            m_playing = false;
            ++m_stats.start_failures;
            return false;
        }
        return true;
    }

    void pause() override {
        std::unique_lock lock(m_mutex);
        AudioQueueRef queue = m_queue;
        const bool should_pause = queue != nullptr && m_queue_running;
        m_playing = false;
        m_queue_running = false;
        lock.unlock();
        if (should_pause) (void)AudioQueuePause(queue);
    }

    void reset() override {
        std::unique_lock lock(m_mutex);
        if (m_queue == nullptr) return;
        AudioQueueRef queue = m_queue;
        m_playing = false;
        lock.unlock();
        (void)AudioQueueStop(queue, true);
        (void)AudioQueueReset(queue);
        lock.lock();
        m_read_frame = m_write_frame = m_padding_frames = 0;
        m_host_read = m_host_write = m_host_frames = 0;
        m_host_segments.clear();
        m_busy.fill(false);
        m_buffer_frames.fill(0u);
        m_buffer_guest_frames.fill(0u);
        m_queued_buffers = m_queued_frames = 0;
        m_queue_running = false;
        m_primed = false;
        m_played_frames = 0;
        m_engine_phase = 0.0;
        m_trim = 0;
        m_drift = {};
    }

    [[nodiscard]] u32 padding_frames() const override {
        std::lock_guard lock(m_mutex);
        return m_padding_frames;
    }

    [[nodiscard]] u64 played_frames() const override {
        std::lock_guard lock(m_mutex);
        return m_played_frames;
    }

    [[nodiscard]] u32 latency_frames() const override {
        std::lock_guard lock(m_mutex);
        return m_latency_frames;
    }

    [[nodiscard]] PushVoiceCounters counters() const override {
        std::lock_guard lock(m_mutex);
        return m_counters;
    }

    [[nodiscard]] bool submit(const u8* frames, u32 frame_count,
                              bool silent) override {
        // Destroyed after `lock`, so the summary prints outside m_mutex.
        StatsLine line;
        const double wait_start = diagnostics::present_cpu::host_mono_ms();
        std::lock_guard lock(m_mutex);
        const double locked_at = diagnostics::present_cpu::host_mono_ms();
        m_stats.submit_wait_max_us =
            std::max(m_stats.submit_wait_max_us, (locked_at - wait_start) * 1000.0);
        if (locked_at - m_stats.window_start_ms >= 1000.0) {
            close_stats_window_locked(line, locked_at);
        }
        if (frame_count > m_capacity_frames - m_padding_frames) return false;
        for (u32 done = 0; done < frame_count;) {
            const u32 run = std::min(frame_count - done, m_capacity_frames - m_write_frame);
            u8* destination = m_endpoint.data() + static_cast<usize>(m_write_frame) * m_align;
            if (silent) std::memset(destination, 0, static_cast<usize>(run) * m_align);
            else std::memcpy(destination, frames + static_cast<usize>(done) * m_align,
                             static_cast<usize>(run) * m_align);
            m_write_frame = (m_write_frame + run) % m_capacity_frames;
            done += run;
        }
        m_padding_frames += frame_count;
        return true;
    }

    void engine_pass(u64 elapsed_ns) override {
        StatsLine line;
        const double wait_start = diagnostics::present_cpu::host_mono_ms();
        std::unique_lock lock(m_mutex);
        const double locked_at = diagnostics::present_cpu::host_mono_ms();
        m_stats.pass_wait_max_us =
            std::max(m_stats.pass_wait_max_us, (locked_at - wait_start) * 1000.0);
        if (locked_at - m_stats.window_start_ms >= 1000.0) {
            close_stats_window_locked(line, locked_at);
        }
        if (!m_playing || m_queue == nullptr) return;
        m_engine_phase += static_cast<double>(elapsed_ns) * 1e-9 *
                          static_cast<double>(m_format.sample_rate) * m_rate_ratio;
        const auto due = static_cast<u64>(m_engine_phase);
        m_engine_phase -= static_cast<double>(due);
        if (m_trim != 0) ++m_stats.trims;
        const u64 trimmed = m_trim < 0 ? due - std::min<u64>(due, 1u) : due + (m_trim > 0 ? 1u : 0u);
        const u32 want = static_cast<u32>(std::min<u64>(trimmed, m_host_capacity));
        const u32 got = std::min(want, m_padding_frames);
        ++m_counters.engine_passes;
        ++m_stats.passes;
        m_stats.pad_min = std::min(m_stats.pad_min, m_padding_frames);
        if (got < want) {
            ++m_counters.short_passes;
            ++m_stats.short_passes;
            m_stats.short_frames += want - got;
        }
        make_host_room_locked(want);
        host_push_endpoint_locked(got);
        host_push_silence_locked(want - got);
        pump_locked();
        const u32 depth = m_host_frames + m_queued_frames;
        m_stats.host_min = std::min(m_stats.host_min, depth);
        m_stats.host_max = std::max(m_stats.host_max, depth);
        if (m_primed) track_drift_locked(depth);
    }

private:
    struct Segment {
        u32 frames;
        bool guest;  ///< client frames, as opposed to underrun silence
    };

    /// Drop the oldest host frames so @p incoming fit (the device stopped
    /// taking frames while the engine kept reading).
    void make_host_room_locked(u32 incoming) {
        if (m_host_frames + incoming <= m_host_capacity) return;
        u32 drop = m_host_frames + incoming - m_host_capacity;
        m_stats.overflow_frames += drop;
        m_host_read = (m_host_read + drop) % m_host_capacity;
        m_host_frames -= drop;
        while (drop != 0 && !m_host_segments.empty()) {
            Segment& front = m_host_segments.front();
            const u32 take = std::min(drop, front.frames);
            front.frames -= take;
            drop -= take;
            if (front.frames == 0) m_host_segments.pop_front();
        }
    }

    void note_host_segment_locked(u32 frames, bool guest) {
        if (frames == 0) return;
        if (!m_host_segments.empty() && m_host_segments.back().guest == guest) {
            m_host_segments.back().frames += frames;
        } else {
            m_host_segments.push_back(Segment{frames, guest});
        }
        m_host_frames += frames;
    }

    void host_push_endpoint_locked(u32 frames) {
        for (u32 done = 0; done < frames;) {
            const u32 run = std::min({frames - done, m_capacity_frames - m_read_frame,
                                      m_host_capacity - m_host_write});
            std::memcpy(m_host.data() + static_cast<usize>(m_host_write) * m_align,
                        m_endpoint.data() + static_cast<usize>(m_read_frame) * m_align,
                        static_cast<usize>(run) * m_align);
            m_read_frame = (m_read_frame + run) % m_capacity_frames;
            m_host_write = (m_host_write + run) % m_host_capacity;
            done += run;
        }
        m_padding_frames -= frames;
        note_host_segment_locked(frames, true);
    }

    void host_push_silence_locked(u32 frames) {
        // WAVE 8-bit PCM is unsigned: its silence is 128.
        const int fill = !m_format.float_samples && m_format.bits_per_sample == 8 ? 0x80 : 0;
        for (u32 done = 0; done < frames;) {
            const u32 run = std::min(frames - done, m_host_capacity - m_host_write);
            std::memset(m_host.data() + static_cast<usize>(m_host_write) * m_align, fill,
                        static_cast<usize>(run) * m_align);
            m_host_write = (m_host_write + run) % m_host_capacity;
            done += run;
        }
        note_host_segment_locked(frames, false);
    }

    /// Move @p frames from the host ring into queue buffer @p index.
    void fill_and_enqueue_locked(usize index, u32 frames) {
        AudioQueueBufferRef buffer = m_buffers[index];
        auto* destination = static_cast<u8*>(buffer->mAudioData);
        for (u32 done = 0; done < frames;) {
            const u32 run = std::min(frames - done, m_host_capacity - m_host_read);
            std::memcpy(destination + static_cast<usize>(done) * m_align,
                        m_host.data() + static_cast<usize>(m_host_read) * m_align,
                        static_cast<usize>(run) * m_align);
            m_host_read = (m_host_read + run) % m_host_capacity;
            done += run;
        }
        m_host_frames -= frames;
        u32 guest = 0;
        for (u32 left = frames; left != 0 && !m_host_segments.empty();) {
            Segment& front = m_host_segments.front();
            const u32 take = std::min(left, front.frames);
            if (front.guest) guest += take;
            front.frames -= take;
            left -= take;
            if (front.frames == 0) m_host_segments.pop_front();
        }
        // Stream volume is applied as the device takes the frames, so a
        // change reaches data already in the host pipeline.
        if (!m_unity_gain) {
            apply_channel_gains(m_format, destination, frames, m_gains.data());
        }
        buffer->mAudioDataByteSize = frames * m_align;
        if (AudioQueueEnqueueBuffer(m_queue, buffer, 0, nullptr) != noErr) {
            m_stats.enqueue_fail_frames += frames;
            return;
        }
        m_busy[index] = true;
        m_buffer_frames[index] = frames;
        m_buffer_guest_frames[index] = guest;
        ++m_queued_buffers;
        m_queued_frames += frames;
    }

    /// Feed free queue buffers from the host ring: whole chunks, or a partial
    /// one only when the device is down to its last chunk. After the device
    /// ran dry nothing is fed until the host ring holds the target again.
    void pump_locked() {
        if (!m_primed) {
            if (m_host_frames < m_target_frames) return;
            m_primed = true;
            m_drift = {};
        }
        for (usize index = 0; index < m_buffers.size(); ++index) {
            if (m_host_frames == 0) break;
            if (m_host_frames < m_chunk_frames && m_queued_frames >= m_chunk_frames) break;
            if (m_busy[index]) continue;
            fill_and_enqueue_locked(index, std::min(m_chunk_frames, m_host_frames));
        }
    }

    /// Engine clock vs device clock: the host pipeline depth after a pass is
    /// constant while both run at the same rate. Its slow average is
    /// referenced once settled; drifting more than one chunk away reads one
    /// frame more or less per pass until it is back.
    void track_drift_locked(u32 depth) {
        Drift& d = m_drift;
        d.ema = d.passes == 0 ? depth : d.ema + (static_cast<double>(depth) - d.ema) / 64.0;
        ++d.passes;
        if (d.passes < 128) return;
        if (d.passes == 128) d.reference = d.ema;
        const double error = d.ema - d.reference;
        const double band = static_cast<double>(m_chunk_frames);
        if (error > band) m_trim = -1;
        else if (error < -band) m_trim = 1;
        else if (std::fabs(error) < band / 4.0) m_trim = 0;
    }

    static void callback(void* context, AudioQueueRef, AudioQueueBufferRef buffer) {
        auto* voice = static_cast<CoreAudioPushVoice*>(context);
        const double wait_start = diagnostics::present_cpu::host_mono_ms();
        std::lock_guard lock(voice->m_mutex);
        const double locked_at = diagnostics::present_cpu::host_mono_ms();
        VoiceStats& stats = voice->m_stats;
        stats.callback_wait_max_us =
            std::max(stats.callback_wait_max_us, (locked_at - wait_start) * 1000.0);
        if (stats.last_callback_ms != 0.0) {
            stats.callback_gap_max_us = std::max(
                stats.callback_gap_max_us, (wait_start - stats.last_callback_ms) * 1000.0);
        }
        stats.last_callback_ms = wait_start;
        if (voice->m_queue == nullptr) return;
        const auto found = std::find(voice->m_buffers.begin(), voice->m_buffers.end(), buffer);
        if (found == voice->m_buffers.end()) return;
        const usize index = static_cast<usize>(found - voice->m_buffers.begin());
        // Buffers flushed by Reset come back unplayed; Reset clears them.
        if (!voice->m_busy[index] || !voice->m_playing) return;
        voice->m_busy[index] = false;
        voice->m_played_frames += voice->m_buffer_guest_frames[index];
        voice->m_queued_frames -= voice->m_buffer_frames[index];
        --voice->m_queued_buffers;
        stats.device_min_frames = std::min(stats.device_min_frames, voice->m_queued_frames);
        if (voice->m_queued_buffers == 0) {
            // The device ran dry. Host frames waiting means the queue refill
            // came too late; none means the engine pass itself was late.
            ++voice->m_counters.device_dry;
            if (voice->m_host_frames != 0) ++stats.dry_fifo;
            else ++stats.dry_starved;
            voice->m_primed = false;
        }
        voice->pump_locked();
    }

    /// Per-second diagnostics, touched only under m_mutex.
    struct VoiceStats {
        double window_start_ms{0.0};
        double last_callback_ms{0.0};
        double callback_gap_max_us{0.0};
        double callback_wait_max_us{0.0};
        double submit_wait_max_us{0.0};
        double pass_wait_max_us{0.0};
        u32 passes{0};
        u32 short_passes{0};
        u64 short_frames{0};
        u32 trims{0};
        u32 dry_fifo{0};
        u32 dry_starved{0};
        u32 starts{0};
        u32 start_failures{0};
        u64 overflow_frames{0};
        u64 enqueue_fail_frames{0};
        u32 device_min_frames{UINT32_MAX};
        u32 host_min{UINT32_MAX};
        u32 host_max{0};
        u32 pad_min{UINT32_MAX};
        u32 windows{0};
    };

    /// A summary line formatted under m_mutex and printed on destruction.
    struct StatsLine {
        char text[768]{};
        StatsLine() = default;
        StatsLine(const StatsLine&) = delete;
        StatsLine& operator=(const StatsLine&) = delete;
        ~StatsLine() {
            if (text[0] != '\0') std::fputs(text, stderr);
        }
    };

    struct Drift {
        u32 passes{0};
        double ema{0.0};
        double reference{0.0};
    };

    /// Close the stats window at @p now_ms. A line is produced when the
    /// device ran dry, the client under-supplied an engine pass, the queue
    /// started, frames were lost, a lock wait exceeded 1 ms or a callback gap
    /// exceeded 4 chunks -- or every tenth window, so a quiet stream still
    /// shows its baseline.
    void close_stats_window_locked(StatsLine& line, double now_ms) {
        VoiceStats& s = m_stats;
        if (s.window_start_ms != 0.0) {
            const double chunk_us = m_format.sample_rate == 0 ? 0.0
                : 1e6 * static_cast<double>(m_chunk_frames) /
                      static_cast<double>(m_format.sample_rate);
            const bool anomaly = s.dry_fifo != 0 || s.dry_starved != 0 ||
                s.short_passes != 0 || s.starts != 0 || s.start_failures != 0 ||
                s.overflow_frames != 0 || s.enqueue_fail_frames != 0 ||
                s.callback_wait_max_us > 1000.0 || s.submit_wait_max_us > 1000.0 ||
                s.pass_wait_max_us > 1000.0 || s.callback_gap_max_us > 4.0 * chunk_us;
            if (anomaly || s.windows % 10 == 0) {
                const auto shown = [](u32 v) { return v == UINT32_MAX ? 0u : v; };
                std::snprintf(line.text, sizeof(line.text),
                    "[coreaudio-voice] v=%p t_ms=%.1f dt_ms=%.0f anom=%d passes=%u "
                    "short=%u short_frames=%llu trims=%u dry_fifo=%u dry_starved=%u "
                    "starts=%u start_fail=%u overflow=%llu enqueue_fail=%llu "
                    "cb_gap_max_us=%.0f dev_min_frames=%u host_min=%u host_max=%u "
                    "pad_min=%u cb_lock_max_us=%.0f submit_lock_max_us=%.0f "
                    "pass_lock_max_us=%.0f playing=%d\n",
                    static_cast<const void*>(this), now_ms, now_ms - s.window_start_ms,
                    anomaly ? 1 : 0, s.passes, s.short_passes,
                    static_cast<unsigned long long>(s.short_frames), s.trims, s.dry_fifo,
                    s.dry_starved, s.starts, s.start_failures,
                    static_cast<unsigned long long>(s.overflow_frames),
                    static_cast<unsigned long long>(s.enqueue_fail_frames),
                    s.callback_gap_max_us, shown(s.device_min_frames), shown(s.host_min),
                    s.host_max, shown(s.pad_min), s.callback_wait_max_us,
                    s.submit_wait_max_us, s.pass_wait_max_us, m_playing ? 1 : 0);
            }
            ++s.windows;
        } else {
            std::snprintf(line.text, sizeof(line.text),
                "[coreaudio-voice] v=%p format rate=%u ch=%u bits=%u float=%d "
                "chunk_frames=%u queue_buffers=%u capacity_frames=%u target_frames=%u "
                "latency_frames=%u (short=engine pass found less than a period in "
                "the endpoint buffer: client late, played as silence; dry_fifo=device "
                "ran dry with host frames waiting: queue refill late; dry_starved="
                "device ran dry, host ring empty: engine pass late; host_*=host "
                "pipeline frames after a pass; pad_min=endpoint padding at a pass)\n",
                static_cast<const void*>(this), m_format.sample_rate,
                m_format.channels, m_format.bits_per_sample,
                m_format.float_samples ? 1 : 0, m_chunk_frames, PUSH_QUEUE_BUFFERS,
                m_capacity_frames, m_target_frames, m_latency_frames);
        }
        const double last_callback = s.last_callback_ms;
        const u32 windows = s.windows;
        s = VoiceStats{};
        s.window_start_ms = now_ms;
        s.last_callback_ms = last_callback;
        s.windows = windows;
    }

    mutable std::mutex m_mutex;
    PcmFormat m_format;
    u32 m_align{0};
    u32 m_capacity_frames{0};
    u32 m_chunk_frames{0};
    u32 m_target_frames{0};
    u32 m_latency_frames{0};
    /// The endpoint buffer: client frames the engine has not read.
    std::vector<u8> m_endpoint;
    u32 m_read_frame{0};
    u32 m_write_frame{0};
    u32 m_padding_frames{0};
    /// The host ring: frames the engine read, not yet in a queue buffer.
    std::vector<u8> m_host;
    u32 m_host_capacity{0};
    u32 m_host_read{0};
    u32 m_host_write{0};
    u32 m_host_frames{0};
    std::deque<Segment> m_host_segments;
    AudioQueueRef m_queue{nullptr};
    std::array<AudioQueueBufferRef, PUSH_QUEUE_BUFFERS> m_buffers{};
    std::array<bool, PUSH_QUEUE_BUFFERS> m_busy{};
    std::array<u32, PUSH_QUEUE_BUFFERS> m_buffer_frames{};
    std::array<u32, PUSH_QUEUE_BUFFERS> m_buffer_guest_frames{};
    u32 m_queued_buffers{0};
    u32 m_queued_frames{0};
    bool m_playing{false};
    bool m_queue_running{false};
    bool m_primed{false};
    u64 m_played_frames{0};
    double m_engine_phase{0.0};
    double m_rate_ratio{1.0};
    int m_trim{0};
    Drift m_drift;
    std::vector<float> m_gains;
    bool m_unity_gain{true};
    bool m_rate_adjust{false};
    PushVoiceCounters m_counters;
    VoiceStats m_stats;
};

class CoreAudioRenderer final : public IAudioRenderer {
public:
    [[nodiscard]] std::unique_ptr<IAudioVoice> create_voice(
        const PcmFormat& format, const u8* ring, u32 ring_bytes) override {
        if (!format.valid() || ring == nullptr || ring_bytes == 0) return nullptr;
        auto voice = std::make_unique<CoreAudioVoice>(format, ring, ring_bytes);
        if (!voice->initialize()) return nullptr;
        VITREO_TRACE("[audio] voice created %uHz %uch %ubit ring=%u bytes\n",
                     format.sample_rate, format.channels, format.bits_per_sample,
                     ring_bytes);
        return voice;
    }

    [[nodiscard]] std::unique_ptr<IAudioPushVoice> create_push_voice(
        const PcmFormat& format, u32 capacity_frames, bool rate_adjust) override {
        if (!format.valid_stream() || capacity_frames == 0) return nullptr;
        auto voice = std::make_unique<CoreAudioPushVoice>(format, capacity_frames,
                                                          rate_adjust);
        return voice->initialize() ? std::move(voice) : nullptr;
    }
};

} // namespace

std::unique_ptr<IAudioRenderer> create_audio_renderer() {
    return std::make_unique<CoreAudioRenderer>();
}

} // namespace vitreo::audio
