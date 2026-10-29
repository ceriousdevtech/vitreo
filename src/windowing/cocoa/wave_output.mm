#include "windowing/contracts/wave_output.hpp"

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vitreo::windowing {
namespace {

class AudioQueueWaveOutput final : public WaveOutput {
public:
    ~AudioQueueWaveOutput() override { close(); }

    u32 device_count() const noexcept override { return 1; }

    bool supports(const WaveOutputFormat& format) const noexcept override {
        return format.channels > 0 && format.channels <= 2 &&
               format.samples_per_second != 0 &&
               (format.bits_per_sample == 8 ||
                format.bits_per_sample == 16 ||
                format.bits_per_sample == 24 ||
                format.bits_per_sample == 32) &&
               format.block_align ==
                   format.channels * (format.bits_per_sample / 8);
    }

    bool open(const WaveOutputFormat& format,
              CompletionCallback completion) override {
        if (!supports(format)) return false;
        close();

        AudioStreamBasicDescription native_format{};
        native_format.mSampleRate = static_cast<Float64>(
            format.samples_per_second);
        native_format.mFormatID = kAudioFormatLinearPCM;
        native_format.mFormatFlags = kLinearPCMFormatFlagIsPacked;
        if (format.bits_per_sample > 8) {
            native_format.mFormatFlags |=
                kLinearPCMFormatFlagIsSignedInteger;
        }
        native_format.mBytesPerPacket = format.block_align;
        native_format.mFramesPerPacket = 1;
        native_format.mBytesPerFrame = format.block_align;
        native_format.mChannelsPerFrame = format.channels;
        native_format.mBitsPerChannel = format.bits_per_sample;

        AudioQueueRef queue = nullptr;
        OSStatus status = AudioQueueNewOutput(
            &native_format, audio_queue_callback, this,
            nullptr, nullptr, 0, &queue);
        if (status != noErr || queue == nullptr) return false;

        m_queue = queue;
        m_silence_sample = format.bits_per_sample == 8 ? 0x80u : 0x00u;
        m_completion = std::move(completion);

        constexpr u32 buffer_count = 4;
        u32 buffer_bytes = std::max<u32>(
            format.block_align, format.average_bytes_per_second / 40u);
        buffer_bytes -= buffer_bytes % format.block_align;
        for (u32 i = 0; i < buffer_count; ++i) {
            AudioQueueBufferRef buffer = nullptr;
            status = AudioQueueAllocateBuffer(
                m_queue, buffer_bytes, &buffer);
            if (status != noErr || buffer == nullptr) {
                close();
                return false;
            }
            std::memset(buffer->mAudioData, m_silence_sample, buffer_bytes);
            buffer->mAudioDataByteSize = buffer_bytes;
            m_buffers.push_back(buffer);
            if (AudioQueueEnqueueBuffer(m_queue, buffer, 0, nullptr) != noErr) {
                close();
                return false;
            }
        }

        m_paused.store(false, std::memory_order_release);
        m_started.store(true, std::memory_order_release);
        if (AudioQueueStart(m_queue, nullptr) != noErr) {
            close();
            return false;
        }
        return true;
    }

    bool is_open() const noexcept override { return m_queue != nullptr; }

    void close() override {
        m_started.store(false, std::memory_order_release);
        AudioQueueRef queue = m_queue;
        m_queue = nullptr;
        if (queue != nullptr) {
            AudioQueueStop(queue, true);
            AudioQueueDispose(queue, true);
        }
        std::lock_guard<std::mutex> guard(m_mutex);
        m_buffers.clear();
        m_pending.clear();
        m_completions.clear();
        m_completion = {};
        m_paused.store(false, std::memory_order_release);
    }

    bool submit(u64 token, std::span<const Byte> pcm) override {
        if (m_queue == nullptr || pcm.empty()) return false;
        PendingBlock block;
        block.token = token;
        block.pcm.assign(pcm.begin(), pcm.end());
        std::lock_guard<std::mutex> guard(m_mutex);
        m_pending.push_back(std::move(block));
        return true;
    }

    void reset() override {
        std::lock_guard<std::mutex> guard(m_mutex);
        m_pending.clear();
        m_completions.clear();
    }

    void set_paused(bool paused) override {
        m_paused.store(paused, std::memory_order_release);
    }

    bool set_volume(float gain) override {
        if (m_queue == nullptr) return false;
        gain = std::clamp(gain, 0.0f, 1.0f);
        return AudioQueueSetParameter(
            m_queue, kAudioQueueParam_Volume, gain) == noErr;
    }

private:
    struct PendingBlock {
        u64 token{0};
        std::vector<Byte> pcm;
        usize offset{0};
    };

    static void audio_queue_callback(void* context, AudioQueueRef queue,
                                     AudioQueueBufferRef buffer) {
        static_cast<AudioQueueWaveOutput*>(context)->refill(queue, buffer);
    }

    void refill(AudioQueueRef queue, AudioQueueBufferRef buffer) {
        std::vector<std::pair<u64, u32>> completed;
        CompletionCallback completion;
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            auto completed_it = m_completions.find(buffer);
            if (completed_it != m_completions.end()) {
                completed = std::move(completed_it->second);
                m_completions.erase(completed_it);
            }
            completion = m_completion;

            auto* output = static_cast<Byte*>(buffer->mAudioData);
            const u32 capacity = buffer->mAudioDataBytesCapacity;
            u32 written = 0;
            auto& next_completions = m_completions[buffer];
            if (!m_paused.load(std::memory_order_acquire)) {
                while (written < capacity && !m_pending.empty()) {
                    auto& block = m_pending.front();
                    const usize remaining =
                        block.pcm.size() - block.offset;
                    const usize count = std::min<usize>(
                        capacity - written, remaining);
                    std::memcpy(output + written,
                                block.pcm.data() + block.offset, count);
                    written += static_cast<u32>(count);
                    block.offset += count;
                    if (block.offset == block.pcm.size()) {
                        next_completions.emplace_back(
                            block.token,
                            static_cast<u32>(block.pcm.size()));
                        m_pending.pop_front();
                    }
                }
            }
            if (written < capacity) {
                std::memset(output + written, m_silence_sample,
                            capacity - written);
            }
            buffer->mAudioDataByteSize = capacity;
        }

        if (completion) {
            for (const auto& [token, bytes] : completed) {
                completion(token, bytes);
            }
        }
        if (m_started.load(std::memory_order_acquire)) {
            (void)AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
        }
    }

    AudioQueueRef m_queue{nullptr};
    std::atomic_bool m_started{false};
    std::atomic_bool m_paused{false};
    u8 m_silence_sample{0};
    std::mutex m_mutex;
    std::vector<AudioQueueBufferRef> m_buffers;
    std::deque<PendingBlock> m_pending;
    std::unordered_map<
        AudioQueueBufferRef, std::vector<std::pair<u64, u32>>> m_completions;
    CompletionCallback m_completion;
};

} // namespace

std::unique_ptr<WaveOutput> create_wave_output() {
    return std::make_unique<AudioQueueWaveOutput>();
}

} // namespace vitreo::windowing
