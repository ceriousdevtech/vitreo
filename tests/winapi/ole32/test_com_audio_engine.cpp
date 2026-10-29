// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_audio_engine.cpp
/// @brief Capture engine model, audio event clock, CoreAudio push voice and channel-gain
/// tests.

#include "com_test_common.hpp"

TEST(CaptureEngineModelTest, PeriodsDeliverTheMeasuredSchedule) {
    // 16 kHz with SRC_DEFAULT_QUALITY (delay 31): "129@480,160@960,...".
    audio::CapturePeriods p{16000, 31, 1, 480};
    EXPECT_EQ(p.end(0), 0u);
    EXPECT_EQ(p.end(1), 129u);
    EXPECT_EQ(p.end(2), 289u);
    EXPECT_EQ(p.period_of(0), 1u);
    EXPECT_EQ(p.period_of(128), 1u);
    EXPECT_EQ(p.period_of(129), 2u);
    EXPECT_EQ(p.boundary_at_or_after(129), 129u);
    EXPECT_EQ(p.boundary_at_or_after(130), 289u);
    EXPECT_EQ(p.position(3), 1440u);
    // 1 kHz: the converter's delay outlasts three periods ("9@1920").
    audio::CapturePeriods slow{1000, 31, 1, 480};
    EXPECT_EQ(slow.end(3), 0u);
    EXPECT_EQ(slow.end(4), 9u);
    EXPECT_EQ(slow.period_of(0), 4u);
    // 11025 fast (delay 11/2): "105,110,111,110,110,110,111".
    audio::CapturePeriods fast{11025, 11, 2, 480};
    const u64 expected[] = {105, 215, 326, 436, 546, 656, 767};
    for (u64 k = 1; k <= 7; ++k) EXPECT_EQ(fast.end(k), expected[k - 1]) << k;
}

TEST(CaptureEngineModelTest, RingResumesAtPeriodBoundaries) {
    audio::CaptureRing ring(2, 100, 48000);
    ring.set_periods(audio::CapturePeriods{48000, 0, 1, 480});
    ring.mark_discontinuity();
    std::vector<u8> frames(2 * 1000, 0);
    ring.push(frames.data(), 150, 1000);  // 100 stored, 50 lost
    EXPECT_EQ(ring.available(), 100u);
    EXPECT_EQ(ring.next_position(), 150u);
    ring.consume(100);
    ring.push(frames.data(), 400, 2000);  // 330 skipped to frame 480, 70 stored
    EXPECT_EQ(ring.available(), 70u);
    EXPECT_EQ(ring.next(100).position, 480u);
    EXPECT_TRUE(ring.next(100).discontinuity);
    // A restart resumes at the next period; Reset keeps the position.
    ring.clear(false);
    ring.reset_stored_total();
    EXPECT_EQ(ring.stored_total(), 0u);
    ring.mark_discontinuity();
    EXPECT_EQ(ring.next_position(), 960u);
}

TEST(CaptureEngineModelTest, ChannelConversionIsExact) {
    EXPECT_TRUE(audio::capture_channels_convertible(1, 2));
    EXPECT_TRUE(audio::capture_channels_convertible(2, 1));
    EXPECT_TRUE(audio::capture_channels_convertible(4, 4));
    EXPECT_FALSE(audio::capture_channels_convertible(4, 2));
    EXPECT_FALSE(audio::capture_channels_convertible(2, 3));
    // Stereo to mono is the mean, per sample format.
    const i16 s16[] = {1000, -3001, -2, 1};
    i16 m16[2] = {};
    audio::convert_capture_channels(audio::PcmFormat{48000, 1, 16, false, 0}, 2,
                                    reinterpret_cast<const u8*>(s16), 2,
                                    reinterpret_cast<u8*>(m16));
    EXPECT_EQ(m16[0], -1001);
    EXPECT_EQ(m16[1], -1);
    const u8 s8[] = {200, 100};
    u8 m8 = 0;
    audio::convert_capture_channels(audio::PcmFormat{48000, 1, 8, false, 0}, 2, s8, 1, &m8);
    EXPECT_EQ(m8, 150u);
    const float sf[] = {0.5f, -0.25f};
    float mf = 0;
    audio::convert_capture_channels(audio::PcmFormat{48000, 1, 32, true, 0}, 2,
                                    reinterpret_cast<const u8*>(sf), 1,
                                    reinterpret_cast<u8*>(&mf));
    EXPECT_FLOAT_EQ(mf, 0.125f);
    const u8 s24[] = {0x00, 0x00, 0x80, 0x00, 0x00, 0x7F};  // -8388608, 8323072
    u8 m24[3] = {};
    audio::convert_capture_channels(audio::PcmFormat{48000, 1, 24, false, 0}, 2, s24, 1, m24);
    const i32 v24 = static_cast<i32>((static_cast<u32>(m24[0]) | (static_cast<u32>(m24[1]) << 8) |
                                      (static_cast<u32>(m24[2]) << 16)) << 8) >> 8;
    EXPECT_EQ(v24, -32768);
    // Mono to stereo copies the sample.
    const i32 s32[] = {123456, -7};
    i32 d32[4] = {};
    audio::convert_capture_channels(audio::PcmFormat{48000, 2, 32, false, 0}, 1,
                                    reinterpret_cast<const u8*>(s32), 2,
                                    reinterpret_cast<u8*>(d32));
    EXPECT_EQ(d32[0], 123456);
    EXPECT_EQ(d32[1], 123456);
    EXPECT_EQ(d32[2], -7);
    EXPECT_EQ(d32[3], -7);
}

TEST(AudioEventClockTest, RemoveIsSynchronousAndStopsSignals) {
    std::atomic<u32> signals{0};
    std::atomic<u64> last{0};
    {
        AudioEventClock clock(std::chrono::milliseconds(1), [&](u64 handle) {
            last.store(handle);
            signals.fetch_add(1);
        });
        clock.add(1, 0x44);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (signals.load() < 3 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_GE(signals.load(), 3u);
        EXPECT_EQ(last.load(), 0x44u);
        clock.add(1, 0x55);  // replace the handle for the same stream
        clock.quiesce();
        const u32 before = signals.load();
        while (signals.load() < before + 2 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_EQ(last.load(), 0x55u);
        clock.remove(1);
        EXPECT_FALSE(clock.contains(1));
        const u32 after = signals.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT_EQ(signals.load(), after);
    }
}

// The host backend must open a queue for every format WASAPI Initialize now
// accepts (PcmFormat::valid_stream), and refuse what it rejects.
TEST(CoreAudioPushVoiceTest, OpensEveryWasapiStreamFormat) {
    auto renderer = audio::create_audio_renderer();
    ASSERT_NE(renderer, nullptr);
    const audio::PcmFormat accepted[] = {
        {48000, 2, 32, true, 0x3},      // the mix format
        {48000, 2, 16, false, 0},
        {48000, 2, 24, false, 0x3},
        {48000, 2, 32, false, 0x3},
        {44100, 1, 8, false, 0},
        {48000, 6, 32, true, 0x3F},     // 5.1 with a speaker mask
        {96000, 8, 32, true, 0x63F},    // 7.1
    };
    for (const auto& format : accepted) {
        auto voice = renderer->create_push_voice(format, 960, false);
        ASSERT_NE(voice, nullptr) << format.channels << "ch " << format.bits_per_sample
                                  << (format.float_samples ? " float" : " int");
        std::vector<u8> frames(static_cast<usize>(format.block_align()) * 16u, 0u);
        EXPECT_TRUE(voice->submit(frames.data(), 16, false));
        EXPECT_EQ(voice->padding_frames(), 16u);
    }
    EXPECT_EQ(renderer->create_push_voice({48000, 2, 64, true, 0}, 960, false), nullptr);
    EXPECT_EQ(renderer->create_push_voice({48000, 2, 16, true, 0}, 960, false), nullptr);
    EXPECT_EQ(renderer->create_push_voice({48000, 9, 16, false, 0}, 960, false), nullptr);
}

// IAudioClockAdjustment rides on the host voice's play rate: only a voice
// opened for rate adjustment takes a ratio, within AudioQueue's 0.5-2.0.
TEST(CoreAudioPushVoiceTest, RateRatioOnlyOnRateAdjustVoices) {
    auto renderer = audio::create_audio_renderer();
    ASSERT_NE(renderer, nullptr);
    const audio::PcmFormat format{48000, 2, 32, true, 0x3};
    auto fixed = renderer->create_push_voice(format, 960, false);
    ASSERT_NE(fixed, nullptr);
    EXPECT_FALSE(fixed->set_rate_ratio(1.01));
    auto adjustable = renderer->create_push_voice(format, 960, true);
    ASSERT_NE(adjustable, nullptr);
    EXPECT_TRUE(adjustable->set_rate_ratio(1.01));
    EXPECT_TRUE(adjustable->set_rate_ratio(0.5));
    EXPECT_TRUE(adjustable->set_rate_ratio(2.0));
    EXPECT_FALSE(adjustable->set_rate_ratio(2.5));
    EXPECT_FALSE(adjustable->set_rate_ratio(0.0));
    EXPECT_FALSE(adjustable->set_rate_ratio(std::numeric_limits<double>::quiet_NaN()));
    // Gains are accepted on any voice; frames still flow.
    const float half[] = {0.5f, 0.25f};
    adjustable->set_channel_gains(half, 2);
    std::vector<u8> frames(format.block_align() * 16u, 0u);
    EXPECT_TRUE(adjustable->submit(frames.data(), 16, false));
    EXPECT_EQ(adjustable->padding_frames(), 16u);
}

namespace {
constexpr u64 kEnginePeriodNs = 10'000'000;  // Learn GetDevicePeriod default: 10 ms
}  // namespace

// Learn (GetCurrentPadding): padding is the endpoint buffer's unread data;
// the engine reads one device period per pass. So after a pass, padding =
// previous padding + frames written - one period, and a pass that finds
// less reads what is there (the client under-supplied).
TEST(CoreAudioPushVoiceTest, EnginePassReadsOnePeriodFromTheEndpointBuffer) {
    auto renderer = audio::create_audio_renderer();
    ASSERT_NE(renderer, nullptr);
    const audio::PcmFormat format{48000, 2, 32, true, 0x3};
    auto voice = renderer->create_push_voice(format, 4800, false);
    ASSERT_NE(voice, nullptr);
    std::vector<u8> period(static_cast<usize>(format.block_align()) * 480u, 0u);
    ASSERT_TRUE(voice->submit(period.data(), 480, false));
    voice->engine_pass(kEnginePeriodNs);  // not started: the engine does not read it
    EXPECT_EQ(voice->padding_frames(), 480u);
    ASSERT_TRUE(voice->start());
    voice->engine_pass(kEnginePeriodNs);
    EXPECT_EQ(voice->padding_frames(), 0u);  // 480 + 0 - 480
    ASSERT_TRUE(voice->submit(period.data(), 480, false));
    ASSERT_TRUE(voice->submit(period.data(), 480, false));
    voice->engine_pass(kEnginePeriodNs);
    EXPECT_EQ(voice->padding_frames(), 480u);  // 0 + 960 - 480
    ASSERT_TRUE(voice->submit(period.data(), 480, false));
    voice->engine_pass(kEnginePeriodNs);
    EXPECT_EQ(voice->padding_frames(), 480u);  // 480 + 480 - 480
    EXPECT_EQ(voice->counters().short_passes, 0u);
    voice->engine_pass(2 * kEnginePeriodNs);   // a late pass owes two periods
    EXPECT_EQ(voice->padding_frames(), 0u);
    EXPECT_EQ(voice->counters().short_passes, 1u);  // 480 of 960 were there
    EXPECT_EQ(voice->counters().engine_passes, 4u);
    // The buffer's free space follows: the whole capacity is writable again.
    std::vector<u8> full(static_cast<usize>(format.block_align()) * 4800u, 0u);
    EXPECT_TRUE(voice->submit(full.data(), 4800, false));
    EXPECT_FALSE(voice->submit(period.data(), 1, false));
    const u32 latency = voice->latency_frames();
    EXPECT_GT(latency, 0u);
    EXPECT_EQ(voice->latency_frames(), latency);
    voice->pause();
    voice->reset();
    EXPECT_EQ(voice->padding_frames(), 0u);
    EXPECT_EQ(voice->played_frames(), 0u);

    // 44.1 kHz: a 10 ms period is 441 frames.
    const audio::PcmFormat cd{44100, 2, 16, false, 0};
    auto cd_voice = renderer->create_push_voice(cd, 4410, false);
    ASSERT_NE(cd_voice, nullptr);
    std::vector<u8> frames(static_cast<usize>(cd.block_align()) * 1000u, 0u);
    ASSERT_TRUE(cd_voice->submit(frames.data(), 1000, false));
    ASSERT_TRUE(cd_voice->start());
    cd_voice->engine_pass(kEnginePeriodNs);
    EXPECT_EQ(cd_voice->padding_frames(), 559u);
    cd_voice->engine_pass(kEnginePeriodNs);
    EXPECT_EQ(cd_voice->padding_frames(), 118u);
}

namespace {
/// A real-time event-driven client on the real CoreAudio device: prefill one
/// period, start, then on every engine event write `frames_per_event`
/// (skipping writes on odd events when `every_other`).
struct EventClientRun {
    audio::PushVoiceCounters counters;
    u64 played{0};
    bool monotonic{true};
    u32 events{0};
};

EventClientRun run_event_client(audio::IAudioPushVoice& voice, const audio::PcmFormat& format,
                                std::chrono::milliseconds duration, bool every_other) {
    std::mutex mutex;
    std::condition_variable cv;
    u64 signalled = 0;
    AudioEventClock clock(std::chrono::nanoseconds(kEnginePeriodNs), [&](u64) {
        {
            std::lock_guard lock(mutex);
            ++signalled;
        }
        cv.notify_one();
    });
    std::vector<u8> period(static_cast<usize>(format.block_align()) * 480u, 0u);
    EventClientRun run;
    EXPECT_TRUE(voice.submit(period.data(), 480, false));
    EXPECT_TRUE(voice.start());
    clock.add(1, 1, &voice);
    // A WASAPI render thread runs as a "Pro Audio"/"Audio" MMCSS task
    // (XAudio2 does); the nearest host class keeps this one on time.
    std::thread client([&] {
        (void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        const auto end = std::chrono::steady_clock::now() + duration;
        u64 seen = 0;
        u64 last_played = 0;
        while (std::chrono::steady_clock::now() < end) {
            std::unique_lock lock(mutex);
            if (!cv.wait_until(lock, end, [&] { return signalled != seen; })) break;
            seen = signalled;
            lock.unlock();
            ++run.events;
            if (!every_other || run.events % 2 == 0) {
                EXPECT_TRUE(voice.submit(period.data(), 480, false));
            }
            const u64 played = voice.played_frames();
            if (played < last_played) run.monotonic = false;
            last_played = played;
        }
        // The client stops writing here, so the stream stops with it.
        clock.remove(1);
    });
    client.join();
    run.counters = voice.counters();
    run.played = voice.played_frames();
    voice.pause();
    return run;
}
}  // namespace

// A client that keeps exactly one period of lead (prefill one period, then
// one period per event) never under-supplies a pass, so the device never
// runs dry and the position advances monotonically at the stream rate.
TEST(CoreAudioPushVoiceTest, OnePeriodLeadPlaysGapFreeOnTheDevice) {
    auto renderer = audio::create_audio_renderer();
    ASSERT_NE(renderer, nullptr);
    const audio::PcmFormat format{48000, 2, 32, true, 0x3};
    auto voice = renderer->create_push_voice(format, 960, false);
    ASSERT_NE(voice, nullptr);
    const EventClientRun run =
        run_event_client(*voice, format, std::chrono::milliseconds(2000), false);
    EXPECT_GE(run.events, 150u);
    EXPECT_EQ(run.counters.short_passes, 0u);
    EXPECT_EQ(run.counters.device_dry, 0u);
    EXPECT_TRUE(run.monotonic);
    // Two seconds less the host latency has played (and never more than was
    // written).
    const u64 latency = voice->latency_frames();
    EXPECT_GE(run.played + latency + 2u * 480u, 2u * 48000u * 9u / 10u);
    EXPECT_LE(run.played, 480u * (run.events + 1u));
}

// Starvation is reported only where the client under-supplies: writing one
// period every other event leaves every other pass short.
TEST(CoreAudioPushVoiceTest, UnderSupplyIsReportedAsShortPasses) {
    auto renderer = audio::create_audio_renderer();
    ASSERT_NE(renderer, nullptr);
    const audio::PcmFormat format{48000, 2, 32, true, 0x3};
    auto voice = renderer->create_push_voice(format, 960, false);
    ASSERT_NE(voice, nullptr);
    const EventClientRun run =
        run_event_client(*voice, format, std::chrono::milliseconds(500), true);
    EXPECT_GE(run.counters.short_passes, run.events / 3u);
    EXPECT_TRUE(run.monotonic);
}

// The gain stage every push voice applies as the device takes frames.
TEST(AudioChannelGainsTest, ScalesEveryWasapiSampleFormat) {
    const float gains[] = {0.5f, 1.0f};
    {   // 8-bit WAVE PCM is unsigned around 128.
        const audio::PcmFormat f{48000, 2, 8, false, 0};
        u8 frames[] = {228, 28, 28, 200};
        audio::apply_channel_gains(f, frames, 2, gains);
        EXPECT_EQ(frames[0], 178u);  // +100 -> +50
        EXPECT_EQ(frames[1], 28u);   // unity: untouched
        EXPECT_EQ(frames[2], 78u);   // -100 -> -50
        EXPECT_EQ(frames[3], 200u);
    }
    {
        const audio::PcmFormat f{48000, 2, 16, false, 0};
        i16 frames[] = {1001, -32768, -3, 7};
        audio::apply_channel_gains(f, reinterpret_cast<u8*>(frames), 2, gains);
        EXPECT_EQ(frames[0], 501);   // 500.5 rounds away from zero
        EXPECT_EQ(frames[1], -32768);
        EXPECT_EQ(frames[2], -2);    // -1.5 rounds away from zero
        EXPECT_EQ(frames[3], 7);
    }
    {   // Packed 24-bit, sign-extended.
        const audio::PcmFormat f{48000, 2, 24, false, 0};
        u8 frames[] = {0x00, 0x00, 0x80, 0x01, 0x02, 0x03};  // -8388608, 0x030201
        audio::apply_channel_gains(f, frames, 1, gains);
        EXPECT_EQ(frames[0], 0x00u);
        EXPECT_EQ(frames[1], 0x00u);
        EXPECT_EQ(frames[2], 0xC0u);  // -4194304
        EXPECT_EQ(frames[3], 0x01u);
        EXPECT_EQ(frames[5], 0x03u);
    }
    {
        const audio::PcmFormat f{48000, 2, 32, false, 0};
        i32 frames[] = {-2000000000, 123};
        audio::apply_channel_gains(f, reinterpret_cast<u8*>(frames), 1, gains);
        EXPECT_EQ(frames[0], -1000000000);
        EXPECT_EQ(frames[1], 123);
    }
    {
        const audio::PcmFormat f{48000, 2, 32, true, 0};
        float frames[] = {0.8f, -0.3f, -1.0f, 0.1f};
        audio::apply_channel_gains(f, reinterpret_cast<u8*>(frames), 2, gains);
        EXPECT_FLOAT_EQ(frames[0], 0.4f);
        EXPECT_FLOAT_EQ(frames[1], -0.3f);
        EXPECT_FLOAT_EQ(frames[2], -0.5f);
        EXPECT_FLOAT_EQ(frames[3], 0.1f);
    }
}
