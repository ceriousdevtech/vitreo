#pragma once
/// @file fake_audio_capture.hpp
/// @brief Host capture backend for tests: scripted devices, permission and
/// streams that deliver frames on demand, so no microphone is needed.

#include <audio/contracts/audio_capture.hpp>

#include <memory>
#include <string>
#include <vector>

namespace vitreo::test {

class FakeCaptureStream final : public audio::IAudioCaptureStream {
public:
    FakeCaptureStream(std::string device, const audio::PcmFormat& format, u32 ring_frames,
                      std::function<void()> on_data)
        : device(std::move(device)), format(format),
          ring_(format.block_align(), ring_frames, format.sample_rate),
          on_data_(std::move(on_data)) {}

    bool start() override {
        ++starts;
        if (!start_ok) return false;
        running = true;
        ring_.mark_discontinuity();
        return true;
    }
    void stop() override {
        ++stops;
        running = false;
    }
    void request_stop() override {
        ++requested_stops;
        running = false;
    }
    audio::CaptureRing& ring() override { return ring_; }

    /// The device delivers `bytes` (whole frames) captured at `qpc`; a
    /// stopped stream delivers nothing, as a stopped AudioQueue does.
    void feed(const std::vector<u8>& bytes, u64 qpc = 1000) {
        if (!running) return;
        ring_.push(bytes.data(), static_cast<u32>(bytes.size() / format.block_align()), qpc);
        if (on_data_) on_data_();
    }
    /// `frames` frames whose every byte is `value`.
    void feed_frames(u32 frames, u8 value, u64 qpc = 1000) {
        feed(std::vector<u8>(static_cast<usize>(frames) * format.block_align(), value), qpc);
    }

    std::string device;
    audio::PcmFormat format;
    bool start_ok{true};
    bool running{false};
    int starts{0};
    int stops{0};
    int requested_stops{0};

private:
    audio::CaptureRing ring_;
    std::function<void()> on_data_;
};

class FakeCapture final : public audio::IAudioCapture {
public:
    std::vector<audio::CaptureDevice> devices() override { return list; }
    std::string default_device() override { return default_uid; }
    audio::CapturePermission permission() override { return state; }
    audio::CapturePermission request_permission() override {
        ++requests;
        if (state == audio::CapturePermission::Undetermined) state = answer;
        return state;
    }
    std::unique_ptr<audio::IAudioCaptureStream> open(const std::string& uid,
                                                     const audio::PcmFormat& format,
                                                     u32 ring_frames,
                                                     std::function<void()> on_data) override {
        ++opens;
        if (fail_open) return {};
        auto stream = std::make_unique<FakeCaptureStream>(uid, format, ring_frames,
                                                          std::move(on_data));
        last = stream.get();
        last_ring_frames = ring_frames;
        return stream;
    }

    /// Two inputs; the default is the second in host order, so "default
    /// first" orderings are observable.
    void add_two_devices() {
        list = {
            audio::CaptureDevice{"uid-usb", u"USB Headset Microphone", 44100, 1, 0x4u},
            audio::CaptureDevice{"uid-builtin", u"MacBook Pro Microphone", 48000, 2, 0x3u},
        };
        default_uid = "uid-builtin";
    }

    std::vector<audio::CaptureDevice> list;
    std::string default_uid;
    audio::CapturePermission state{audio::CapturePermission::Granted};
    audio::CapturePermission answer{audio::CapturePermission::Granted};
    bool fail_open{false};
    int requests{0};
    int opens{0};
    u32 last_ring_frames{0};
    FakeCaptureStream* last{nullptr};
};

}  // namespace vitreo::test
