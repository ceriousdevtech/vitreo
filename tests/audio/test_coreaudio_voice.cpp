// SPDX-License-Identifier: LGPL-2.1-only
//
// The CoreAudio ring-buffer voice behind DirectSound secondary buffers.
//
// IDirectSoundBuffer::Play / Stop / SetCurrentPosition change the buffer's
// state when they return (GetStatus reports DSBSTATUS_PLAYING straight after
// Play) and never wait for the audio device: DirectSound mixes
// asynchronously.
// https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee418074(v=vs.85)
// https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee418062(v=vs.85)
// These run against the real host output, and skip where none exists.

#include "audio/contracts/audio_renderer.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace {

using vitreo::audio::create_audio_renderer;
using vitreo::audio::IAudioRenderer;
using vitreo::audio::IAudioVoice;
using vitreo::audio::PcmFormat;
using Clock = std::chrono::steady_clock;

class CoreAudioVoiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        renderer_ = create_audio_renderer();
        ASSERT_NE(renderer_, nullptr);
    }

    /// 16-bit stereo 22050 Hz voice over @p seconds of silence.
    std::unique_ptr<IAudioVoice> make_voice(double seconds) {
        format_.sample_rate = 22050;
        format_.channels = 2;
        format_.bits_per_sample = 16;
        ring_.assign(static_cast<std::size_t>(seconds * format_.sample_rate) *
                         format_.block_align(),
                     0);
        return renderer_->create_voice(format_, ring_.data(),
                                       static_cast<vitreo::u32>(ring_.size()));
    }

    PcmFormat format_{};
    std::vector<vitreo::u8> ring_;
    std::unique_ptr<IAudioRenderer> renderer_;
};

#define REQUIRE_VOICE(v)                                                    \
    if (!(v)) GTEST_SKIP() << "no host audio output queue on this machine"

// Play reports PLAYING on return, and the calls a game makes from its frame
// loop for every sound effect return without waiting for the device. The
// bound is generous (a device IO cycle is ~10 ms, and the old synchronous
// start + prime routinely waited a cycle or more per call).
TEST_F(CoreAudioVoiceTest, PlayIsImmediateAndDoesNotWaitForTheDevice) {
    auto voice = make_voice(1.0);
    REQUIRE_VOICE(voice);

    constexpr int kSounds = 20;
    const auto begin = Clock::now();
    for (int i = 0; i < kSounds; ++i) {
        ASSERT_TRUE(voice->play(false));
        EXPECT_TRUE(voice->is_playing());
        voice->stop();
        EXPECT_FALSE(voice->is_playing());
    }
    const auto per_call = (Clock::now() - begin) / kSounds;
    EXPECT_LT(per_call, std::chrono::milliseconds(2))
        << "Play+Stop took "
        << std::chrono::duration<double, std::milli>(per_call).count()
        << " ms per pair";
}

// Until the device has started, the play cursor is where Play started.
TEST_F(CoreAudioVoiceTest, CursorStartsAtThePlayPosition) {
    auto voice = make_voice(1.0);
    REQUIRE_VOICE(voice);
    const vitreo::u32 start = format_.block_align() * 1000;
    voice->set_position(start);
    EXPECT_EQ(voice->play_cursor(), start);
    ASSERT_TRUE(voice->play(true));
    const vitreo::u32 cursor = voice->play_cursor();
    EXPECT_GE(cursor, start);
    EXPECT_LT(cursor, static_cast<vitreo::u32>(ring_.size()));
}

// A stopped buffer keeps its position; Play resumes from it.
TEST_F(CoreAudioVoiceTest, StopFreezesTheCursorAndPlayResumesThere) {
    auto voice = make_voice(1.0);
    REQUIRE_VOICE(voice);
    ASSERT_TRUE(voice->play(true));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    voice->stop();
    const vitreo::u32 frozen = voice->play_cursor();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(voice->play_cursor(), frozen);
    EXPECT_EQ(voice->write_cursor(), frozen);
    ASSERT_TRUE(voice->play(true));
    EXPECT_GE(voice->play_cursor(), frozen);
}

// A non-looping buffer stops by itself once played out.
TEST_F(CoreAudioVoiceTest, NonLoopingBufferStopsAtItsEnd) {
    auto voice = make_voice(0.05);
    REQUIRE_VOICE(voice);
    ASSERT_TRUE(voice->play(false));
    EXPECT_TRUE(voice->is_playing());
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (voice->is_playing() && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_FALSE(voice->is_playing());
}

// SetCurrentPosition on a playing buffer keeps it playing from the new spot.
TEST_F(CoreAudioVoiceTest, SetPositionWhilePlayingKeepsPlaying) {
    auto voice = make_voice(1.0);
    REQUIRE_VOICE(voice);
    ASSERT_TRUE(voice->play(true));
    const vitreo::u32 target = format_.block_align() * 5000;
    voice->set_position(target);
    EXPECT_TRUE(voice->is_playing());
    EXPECT_GE(voice->play_cursor(), target);
}

// Pause then Play resumes; a voice destroyed with control operations still
// in flight neither hangs nor touches freed state.
TEST_F(CoreAudioVoiceTest, PauseResumeAndDestroyWithWorkInFlight) {
    auto voice = make_voice(1.0);
    REQUIRE_VOICE(voice);
    ASSERT_TRUE(voice->play(true));
    voice->pause();
    EXPECT_FALSE(voice->is_playing());
    ASSERT_TRUE(voice->play(true));
    EXPECT_TRUE(voice->is_playing());
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(voice->play(false));
        voice->set_position(0);
        voice->stop();
    }
    voice.reset();  // must drain the control queue, then dispose
    SUCCEED();
}

}  // namespace
