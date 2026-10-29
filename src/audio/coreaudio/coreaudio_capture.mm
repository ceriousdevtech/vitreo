/// @file coreaudio_capture.mm
/// @brief macOS implementation of the audio capture contract: CoreAudio HAL
/// device enumeration, AVFoundation microphone permission, and AudioQueue
/// input streams that convert to the format the frontend asks for.

#include "audio/contracts/audio_capture.hpp"
#include "platform/macos/tcc_responsibility.hpp"

#include <vitreo/virtual_tsc.hpp>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <cstdio>
#include <thread>
#include <unistd.h>
#include <vector>

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

namespace vitreo::audio {
namespace {

/// AudioQueue buffers kept in flight, each one 10 ms engine period long.
constexpr u32 kQueueBuffers = 4;
constexpr double kQueueBufferSeconds = 0.010;

std::string cf_to_utf8(CFStringRef text) {
    if (text == nullptr) return {};
    const CFIndex length = CFStringGetLength(text);
    const CFIndex max = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<usize>(max), '\0');
    if (!CFStringGetCString(text, out.data(), max, kCFStringEncodingUTF8)) return {};
    out.resize(std::strlen(out.c_str()));
    return out;
}

std::u16string cf_to_utf16(CFStringRef text) {
    if (text == nullptr) return {};
    const CFIndex length = CFStringGetLength(text);
    std::u16string out(static_cast<usize>(length), u'\0');
    CFStringGetCharacters(text, CFRangeMake(0, length), reinterpret_cast<UniChar*>(out.data()));
    return out;
}

template <typename T>
bool get_property(AudioObjectID object, AudioObjectPropertySelector selector,
                  AudioObjectPropertyScope scope, T& out) {
    AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &out) == noErr;
}

u32 input_channels(AudioDeviceID device) {
    AudioObjectPropertyAddress address{kAudioDevicePropertyStreamConfiguration,
                                       kAudioDevicePropertyScopeInput,
                                       kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr ||
        size == 0) {
        return 0;
    }
    std::vector<u8> storage(size);
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, list) != noErr) return 0;
    u32 channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) channels += list->mBuffers[i].mNumberChannels;
    return channels;
}

/// The WAVEFORMATEXTENSIBLE speaker mask of the device's input channels, in
/// order. CoreAudio's labels 1..18 (Left .. TopBackRight) are the speaker
/// positions whose AudioChannelBitmap bits are the SPEAKER_* bits
/// (CoreAudioBaseTypes.h annotates the WAVE names), so label L is bit L-1;
/// a mono input labelled Mono is SPEAKER_FRONT_CENTER (KSAUDIO_SPEAKER_MONO).
/// A mask must list the channels in ascending bit order; any channel without
/// a position, a repeat or an out-of-order label leaves the layout unnamed,
/// and the input is described by its channel count alone.
u32 input_channel_mask(AudioDeviceID device, u32 channels) {
    AudioObjectPropertyAddress address{kAudioDevicePropertyPreferredChannelLayout,
                                       kAudioDevicePropertyScopeInput,
                                       kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) == noErr &&
        size >= offsetof(AudioChannelLayout, mChannelDescriptions)) {
        std::vector<u8> storage(size);
        auto* layout = reinterpret_cast<AudioChannelLayout*>(storage.data());
        if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, layout) == noErr &&
            layout->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelDescriptions &&
            layout->mNumberChannelDescriptions == channels) {
            u32 mask = 0;
            u32 last_bit = 0;
            bool named = true;
            for (UInt32 i = 0; i < channels && named; ++i) {
                const AudioChannelLabel label = layout->mChannelDescriptions[i].mChannelLabel;
                u32 bit = 0;
                if (label >= kAudioChannelLabel_Left && label <= kAudioChannelLabel_TopBackRight) {
                    bit = 1u << (label - 1u);
                } else if (label == kAudioChannelLabel_Mono && channels == 1) {
                    bit = 0x4u;
                }
                named = bit != 0 && bit > last_bit;
                last_bit = bit;
                mask |= bit;
            }
            if (named) return mask;
        }
    }
    return unlabelled_capture_channel_mask(static_cast<u16>(channels));
}

AudioDeviceID device_for_uid(const std::string& uid) {
    CFStringRef text = CFStringCreateWithCString(nullptr, uid.c_str(), kCFStringEncodingUTF8);
    if (text == nullptr) return kAudioObjectUnknown;
    AudioDeviceID id = kAudioObjectUnknown;
    AudioObjectPropertyAddress address{kAudioHardwarePropertyTranslateUIDToDevice,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    UInt32 size = sizeof(id);
    const OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address,
                                                       sizeof(text), &text, &size, &id);
    CFRelease(text);
    return status == noErr ? id : kAudioObjectUnknown;
}

std::string device_uid(AudioDeviceID device) {
    CFStringRef uid = nullptr;
    if (!get_property(device, kAudioDevicePropertyDeviceUID,
                      kAudioObjectPropertyScopeGlobal, uid) || uid == nullptr) {
        return {};
    }
    std::string out = cf_to_utf8(uid);
    CFRelease(uid);
    return out;
}

/// The interleaved linear PCM description of a PcmFormat.
AudioStreamBasicDescription describe(const PcmFormat& format) {
    AudioStreamBasicDescription d{};
    d.mSampleRate = format.sample_rate;
    d.mFormatID = kAudioFormatLinearPCM;
    d.mChannelsPerFrame = format.channels;
    d.mBitsPerChannel = format.bits_per_sample;
    d.mBytesPerFrame = format.block_align();
    d.mFramesPerPacket = 1;
    d.mBytesPerPacket = d.mBytesPerFrame;
    d.mFormatFlags = kAudioFormatFlagIsPacked;
    if (format.float_samples) {
        d.mFormatFlags |= kAudioFormatFlagIsFloat;
    } else if (format.bits_per_sample > 8) {
        // WAVE 8-bit PCM is unsigned; wider integer samples are signed.
        d.mFormatFlags |= kAudioFormatFlagIsSignedInteger;
    }
    return d;
}

class CoreAudioCaptureStream final : public IAudioCaptureStream {
public:
    /// The queue records the device's own channels in the stream's sample
    /// format and rate; the callback converts them to the stream's channels
    /// (convert_capture_channels).
    CoreAudioCaptureStream(const PcmFormat& format, u16 device_channels, u32 ring_frames,
                           std::function<void()> on_data)
        : m_format(format),
          m_device_format(with_channels(format, device_channels)),
          m_ring(format.block_align(), ring_frames, format.sample_rate),
          m_on_data(std::move(on_data)) {}

    ~CoreAudioCaptureStream() override {
        stop();
        if (m_queue != nullptr) (void)AudioQueueDispose(m_queue, true);
    }

    [[nodiscard]] bool create(const std::string& uid) {
        const AudioStreamBasicDescription desc = describe(m_device_format);
        if (AudioQueueNewInput(&desc, &CoreAudioCaptureStream::callback, this, nullptr,
                               nullptr, 0, &m_queue) != noErr) {
            std::fprintf(stderr, "[audio] AudioQueueNewInput failed rate=%u ch=%u bits=%u "
                                 "float=%d\n",
                         m_format.sample_rate, m_format.channels,
                         m_format.bits_per_sample, m_format.float_samples ? 1 : 0);
            m_queue = nullptr;
            return false;
        }
        CFStringRef device = CFStringCreateWithCString(nullptr, uid.c_str(),
                                                       kCFStringEncodingUTF8);
        const OSStatus selected = device == nullptr ? -1
            : AudioQueueSetProperty(m_queue, kAudioQueueProperty_CurrentDevice,
                                    &device, sizeof(device));
        if (device != nullptr) CFRelease(device);
        if (selected != noErr) return false;
        const u32 frames = std::max<u32>(
            1, static_cast<u32>(m_format.sample_rate * kQueueBufferSeconds));
        for (u32 i = 0; i < kQueueBuffers; ++i) {
            AudioQueueBufferRef buffer = nullptr;
            if (AudioQueueAllocateBuffer(m_queue, frames * m_device_format.block_align(),
                                         &buffer) != noErr) {
                return false;
            }
            m_buffers.push_back(buffer);
        }
        if (m_device_format.channels != m_format.channels) {
            m_converted.resize(static_cast<usize>(frames) * m_format.block_align());
        }
        return true;
    }

    bool start() override {
        if (m_queue == nullptr) return false;
        if (m_running.load()) return true;
        // Settle any earlier stop (an asynchronous one requested from the
        // data callback included) and take every buffer back before
        // re-priming the queue.
        (void)AudioQueueStop(m_queue, true);
        (void)AudioQueueReset(m_queue);
        for (AudioQueueBufferRef buffer : m_buffers) {
            if (AudioQueueEnqueueBuffer(m_queue, buffer, 0, nullptr) != noErr) return false;
        }
        m_ring.mark_discontinuity();
        m_running.store(true);
        if (AudioQueueStart(m_queue, nullptr) != noErr) {
            m_running.store(false);
            (void)AudioQueueReset(m_queue);
            return false;
        }
        return true;
    }

    void stop() override {
        if (m_queue == nullptr) return;
        m_running.store(false);
        // Synchronous: no input callback runs after this returns.
        (void)AudioQueueStop(m_queue, true);
    }

    void request_stop() override {
        if (m_queue == nullptr || !m_running.exchange(false)) return;
        // Asynchronous: allowed from the input callback itself.
        (void)AudioQueueStop(m_queue, false);
    }

    CaptureRing& ring() override { return m_ring; }

private:
    static void callback(void* user, AudioQueueRef queue, AudioQueueBufferRef buffer,
                         const AudioTimeStamp*, UInt32, const AudioStreamPacketDescription*) {
        auto* self = static_cast<CoreAudioCaptureStream*>(user);
        if (!self->m_running.load()) return;
        const u32 align = self->m_device_format.block_align();
        u32 frames = align == 0 ? 0 : buffer->mAudioDataByteSize / align;
        const u8* data = static_cast<const u8*>(buffer->mAudioData);
        if (frames != 0 && self->m_device_format.channels != self->m_format.channels) {
            frames = std::min<u32>(frames, static_cast<u32>(self->m_converted.size() /
                                                            self->m_format.block_align()));
            convert_capture_channels(self->m_format, self->m_device_format.channels, data,
                                     frames, self->m_converted.data());
            data = self->m_converted.data();
        }
        if (frames != 0) {
            // The frames end now: the first one was sampled `frames` earlier.
            const u64 now = virtual_qpc_ticks();
            const u64 span = static_cast<u64>(frames) * 10'000'000ull /
                             std::max<u32>(self->m_format.sample_rate, 1);
            self->m_ring.push(data, frames, now > span ? now - span : 0);
            if (self->m_on_data) self->m_on_data();
        }
        if (self->m_running.load()) (void)AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    }

    static PcmFormat with_channels(PcmFormat format, u16 channels) {
        format.channels = channels;
        return format;
    }

    PcmFormat m_format;
    PcmFormat m_device_format;
    std::vector<u8> m_converted;
    CaptureRing m_ring;
    std::function<void()> m_on_data;
    AudioQueueRef m_queue{nullptr};
    std::vector<AudioQueueBufferRef> m_buffers;
    std::atomic<bool> m_running{false};
};

class CoreAudioCapture final : public IAudioCapture {
    /// The code TCC judges for this process's microphone requests.
    struct ResponsibleCode {
        pid_t pid{-1};
        bool self{false};
        std::string executable;
        bool declares_usage{false};
    };
    static ResponsibleCode responsible_code() {
        ResponsibleCode code;
        code.pid = platform::responsible_process(::getpid());
        if (code.pid <= 0) code.pid = ::getpid();
        code.self = code.pid == ::getpid();
        code.executable = platform::process_executable_path(code.pid);
        code.declares_usage =
            platform::code_declares_usage(code.executable, "NSMicrophoneUsageDescription");
        return code;
    }

public:
    std::vector<CaptureDevice> devices() override {
        std::vector<CaptureDevice> out;
        AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices,
                                           kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain};
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr,
                                           &size) != noErr) {
            return out;
        }
        std::vector<AudioDeviceID> ids(size / sizeof(AudioDeviceID));
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                       ids.data()) != noErr) {
            return out;
        }
        for (const AudioDeviceID id : ids) {
            const u32 channels = input_channels(id);
            if (channels == 0) continue;
            CaptureDevice device;
            device.uid = device_uid(id);
            if (device.uid.empty()) continue;
            CFStringRef name = nullptr;
            if (get_property(id, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                             name) && name != nullptr) {
                device.name = cf_to_utf16(name);
                CFRelease(name);
            }
            // A device whose rate or channel count cannot be described is
            // not offered (no value is invented for it).
            Float64 rate = 0;
            if (!get_property(id, kAudioDevicePropertyNominalSampleRate,
                              kAudioObjectPropertyScopeGlobal, rate) ||
                !(rate >= 1.0 && rate <= 4.0e9) || channels > 0xFFFFu) {
                continue;
            }
            device.sample_rate = static_cast<u32>(std::lround(rate));
            device.channels = static_cast<u16>(channels);
            device.channel_mask = input_channel_mask(id, channels);
            out.push_back(std::move(device));
        }
        return out;
    }

    std::string default_device() override {
        AudioDeviceID id = kAudioObjectUnknown;
        if (!get_property(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultInputDevice,
                          kAudioObjectPropertyScopeGlobal, id) ||
            id == kAudioObjectUnknown || input_channels(id) == 0) {
            return {};
        }
        return device_uid(id);
    }

    CapturePermission permission() override {
        switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
        case AVAuthorizationStatusAuthorized: return CapturePermission::Granted;
        case AVAuthorizationStatusNotDetermined: return CapturePermission::Undetermined;
        case AVAuthorizationStatusDenied:
        case AVAuthorizationStatusRestricted:
        default: return CapturePermission::Denied;
        }
    }

    std::string permission_context() override {
        const ResponsibleCode code = responsible_code();
        std::string text = "TCC judges ";
        text += code.self ? "this process" : "the launching process";
        text += " (pid " + std::to_string(code.pid) + ", " +
                (code.executable.empty() ? std::string("executable unknown") : code.executable) +
                "), whose Info.plist ";
        text += code.declares_usage ? "declares" : "lacks";
        text += " NSMicrophoneUsageDescription";
        return text;
    }

    CapturePermission request_permission() override {
        if (permission() != CapturePermission::Undetermined) return permission();
        // TCC asks on behalf of the responsible process (a guest app bundle
        // re-executed responsible for itself is judged as the vitreo
        // executable, whose embedded Info.plist carries the usage string;
        // see platform/macos/tcc_responsibility.hpp). When that code
        // declares no NSMicrophoneUsageDescription, TCC does not answer: it
        // terminates the process. Refuse instead.
        if (!responsible_code().declares_usage) return CapturePermission::Denied;
        // The request is made from a worker thread, never the host main
        // thread, and the caller waits for the user's answer.
        struct Answer {
            std::mutex mutex;
            std::condition_variable ready;
            int value{-1};
        };
        auto answer = std::make_shared<Answer>();
        std::thread asker([answer] {
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                     completionHandler:^(BOOL granted) {
                std::lock_guard lock(answer->mutex);
                answer->value = granted ? 1 : 0;
                answer->ready.notify_all();
            }];
        });
        asker.join();
        std::unique_lock lock(answer->mutex);
        if ([NSThread isMainThread]) {
            // A guest call that arrived on the host main thread keeps its run
            // loop turning while the answer is pending.
            while (answer->value < 0) {
                lock.unlock();
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
                lock.lock();
            }
        } else {
            answer->ready.wait(lock, [&] { return answer->value >= 0; });
        }
        return answer->value == 1 ? CapturePermission::Granted : CapturePermission::Denied;
    }

    std::unique_ptr<IAudioCaptureStream> open(const std::string& uid, const PcmFormat& format,
                                              u32 ring_frames,
                                              std::function<void()> on_data) override {
        if (!format.valid_stream(kCaptureMaxChannels) || ring_frames == 0) return {};
        const AudioDeviceID id = device_for_uid(uid);
        if (id == kAudioObjectUnknown) return {};
        const u32 channels = input_channels(id);
        if (channels == 0 || channels > kCaptureMaxChannels ||
            !capture_channels_convertible(static_cast<u16>(channels), format.channels)) {
            return {};
        }
        auto stream = std::make_unique<CoreAudioCaptureStream>(
            format, static_cast<u16>(channels), ring_frames, std::move(on_data));
        if (!stream->create(uid)) return {};
        return stream;
    }
};

}  // namespace

std::unique_ptr<IAudioCapture> create_audio_capture() {
    return std::make_unique<CoreAudioCapture>();
}

}  // namespace vitreo::audio
