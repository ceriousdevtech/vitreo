#include "audio/contracts/audio_renderer.hpp"
#include "audio/contracts/media_player.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace vitreo::audio {
namespace {
// This binary links the decoder to a controlled host device, rather than the
// CoreAudio backend. The real decoder and graph clock remain under test.
std::atomic<bool> start_succeeds{true};
std::atomic<bool> play_entered{false};
std::atomic<bool> create_entered{false};
std::atomic<int> create_delay_ms{0};
std::atomic<bool> cursor_entered{false};
std::atomic<int> cursor_delay_ms{0};
class SlowStartVoice final : public IAudioVoice {
public:
    bool play(bool) override {
        play_entered.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        playing_ = start_succeeds.load();
        return playing_;
    }
    void pause() override { playing_ = false; }
    void stop() override { playing_ = false; }
    bool is_playing() const override { return playing_; }
    u32 play_cursor() const override {
        cursor_entered.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(cursor_delay_ms.load()));
        return 0;
    }
    u32 write_cursor() const override { return 0; }
    void set_position(u32) override {}
    void set_volume(i32) override {}
    void set_pan(i32) override {}
    void set_frequency(u32) override {}
private:
    bool playing_{false};
};
class SlowStartRenderer final : public IAudioRenderer {
public:
    std::unique_ptr<IAudioVoice> create_voice(const PcmFormat&, const u8*, u32) override {
        create_entered.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(create_delay_ms.load()));
        return std::make_unique<SlowStartVoice>();
    }
};
} // namespace
std::unique_ptr<IAudioRenderer> create_audio_renderer() {
    return std::make_unique<SlowStartRenderer>();
}
} // namespace vitreo::audio

TEST(MediaStartupTest, SinkInitializationTimeIsNotRenderedVideoTime) {
    using namespace std::chrono_literals;
    using namespace vitreo::audio;
    char directory[] = "/tmp/vitreo_media_startup_XXXXXX";
    ASSERT_NE(::mkdtemp(directory), nullptr);
    const std::filesystem::path root(directory);
    const auto path = root / "clip.mp4";
    const std::string command =
        "ffmpeg -y -hide_banner -loglevel error -f lavfi "
        "-i color=c=red:s=16x16:d=0.4:r=10 -f lavfi "
        "-i sine=frequency=440:duration=0.4 -pix_fmt yuv420p -shortest '" +
        path.string() + "'";
    if (std::system(command.c_str()) != 0) {
        std::filesystem::remove_all(root);
        GTEST_SKIP() << "ffmpeg could not generate the native media fixture";
    }
    // A failed connected sink must report failure, even with a video stream.
    // Learn Run: a filter failure is not S_OK for the whole graph.
    for (bool succeeds : {true, false}) {
        SCOPED_TRACE(succeeds);
        start_succeeds.store(succeeds);
        auto player = create_media_player();
        ASSERT_NE(player, nullptr);
        ASSERT_TRUE(player->open(path.string()));
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!player->ready() && !player->failed() &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        ASSERT_TRUE(player->ready());
        ASSERT_EQ(player->run(), succeeds ? MediaStartResult::Started : MediaStartResult::Failed);
        // Learn Run/EC_COMPLETE: startup has not rendered any samples. The
        // 500 ms host initialization exceeds this 400 ms source's duration.
        EXPECT_LT(player->position_100ns(), 1'000'000);
        EXPECT_FALSE(player->take_completed());
    }
    std::filesystem::remove_all(root);
}

TEST(MediaStartupTest, PauseStopAndRepeatedRunPreserveTheRenderedPosition) {
    using namespace std::chrono_literals;
    using namespace vitreo::audio;
    char directory[] = "/tmp/vitreo_media_clock_XXXXXX";
    ASSERT_NE(::mkdtemp(directory), nullptr);
    const std::filesystem::path root(directory);
    const auto path = root / "clip.mp4";
    const std::string command = "ffmpeg -y -hide_banner -loglevel error -f lavfi "
        "-i color=c=red:s=16x16:d=2:r=10 -f lavfi -i sine=frequency=440:duration=2 "
        "-pix_fmt yuv420p -shortest '" + path.string() + "'";
    ASSERT_EQ(std::system(command.c_str()), 0);
    start_succeeds.store(true);
    auto player = create_media_player(); ASSERT_NE(player, nullptr);
    ASSERT_TRUE(player->open(path.string()));
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!player->ready() && !player->failed() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    ASSERT_TRUE(player->ready()); ASSERT_EQ(player->run(), MediaStartResult::Started);
    std::this_thread::sleep_for(80ms);
    player->pause();
    const auto paused = player->position_100ns();
    EXPECT_GE(paused, 500'000);
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(player->position_100ns(), paused); // Learn get_CurrentPosition
    ASSERT_EQ(player->run(), MediaStartResult::Started);
    EXPECT_GE(player->position_100ns(), paused);
    EXPECT_LT(player->position_100ns(), paused + 1'000'000); // sink startup is excluded on resume too
    const auto repeated_start = std::chrono::steady_clock::now();
    ASSERT_EQ(player->run(), MediaStartResult::Started); // Learn Run: already running has no effect
    EXPECT_LT(std::chrono::steady_clock::now() - repeated_start, 100ms);
    player->stop();
    const auto stopped = player->position_100ns();
    EXPECT_GE(stopped, paused); // Learn Stop does not seek
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(player->position_100ns(), stopped);
    ASSERT_EQ(player->run(), MediaStartResult::Started);
    EXPECT_GE(player->position_100ns(), stopped);
    EXPECT_LT(player->position_100ns(), stopped + 1'000'000);
    std::filesystem::remove_all(root);
}

TEST(MediaStartupTest, CompletionIsOncePerRunningTransitionAndRearmedByPause) {
    using namespace std::chrono_literals;
    using namespace vitreo::audio;
    char directory[] = "/tmp/vitreo_media_complete_XXXXXX";
    ASSERT_NE(::mkdtemp(directory), nullptr);
    const std::filesystem::path root(directory);
    const auto path = root / "clip.mp4";
    const std::string command = "ffmpeg -y -hide_banner -loglevel error -f lavfi "
        "-i color=c=red:s=16x16:d=0.4:r=10 -f lavfi -i sine=frequency=440:duration=0.4 "
        "-pix_fmt yuv420p -shortest '" + path.string() + "'";
    ASSERT_EQ(std::system(command.c_str()), 0);
    start_succeeds.store(true);
    auto player = create_media_player(); ASSERT_NE(player, nullptr);
    ASSERT_TRUE(player->open(path.string()));
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!player->ready() && !player->failed() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    ASSERT_TRUE(player->ready()); ASSERT_EQ(player->run(), MediaStartResult::Started);
    std::this_thread::sleep_for(450ms);
    EXPECT_TRUE(player->take_completed());
    EXPECT_FALSE(player->take_completed()); // Learn EC_COMPLETE: exactly one per stream
    ASSERT_EQ(player->run(), MediaStartResult::Started);
    EXPECT_FALSE(player->take_completed()); // no transition
    player->pause();
    EXPECT_FALSE(player->take_completed());
    ASSERT_EQ(player->run(), MediaStartResult::Started);
    EXPECT_TRUE(player->take_completed()); // pause->run at EOS sends again
    EXPECT_FALSE(player->take_completed());
    player->pause(); ASSERT_EQ(player->run(), MediaStartResult::Started);
    player->pause(); ASSERT_EQ(player->run(), MediaStartResult::Started);
    EXPECT_TRUE(player->take_completed()); EXPECT_TRUE(player->take_completed());
    EXPECT_FALSE(player->take_completed()); // two transitions, two pending events
    std::filesystem::remove_all(root);
}

TEST(MediaStartupTest, VideoOnlyHasNoAudioFailureAndSeekFlushesThePicture) {
    using namespace std::chrono_literals;
    using namespace vitreo::audio;
    char directory[] = "/tmp/vitreo_media_seek_XXXXXX";
    ASSERT_NE(::mkdtemp(directory), nullptr);
    const std::filesystem::path root(directory);
    const auto path = root / "clip.mp4";
    const std::string command = "ffmpeg -y -hide_banner -loglevel error -f lavfi "
        "-i color=c=red:s=16x16:d=1:r=10 -f lavfi -i color=c=blue:s=16x16:d=1:r=10 "
        "-filter_complex '[0:v][1:v]concat=n=2:v=1:a=0[v]' -map '[v]' -an -pix_fmt yuv420p '" + path.string() + "'";
    ASSERT_EQ(std::system(command.c_str()), 0);
    start_succeeds.store(false); // a video-only file must not create an audio sink
    auto player = create_media_player(); ASSERT_NE(player, nullptr);
    ASSERT_TRUE(player->open(path.string()));
    ASSERT_TRUE(player->ready());
    EXPECT_FALSE(player->failed());
    std::vector<vitreo::u8> pixels; vitreo::u32 width = 0, height = 0; vitreo::u64 frame = 0;
    ASSERT_TRUE(player->copy_video_frame(pixels, width, height, frame));
    ASSERT_EQ(pixels.size(), 1024u); EXPECT_GT(pixels[0], 200u); EXPECT_LT(pixels[2], 80u);
    ASSERT_TRUE(player->seek_100ns(12'000'000));
    EXPECT_EQ(player->position_100ns(), 12'000'000);
    ASSERT_TRUE(player->copy_video_frame(pixels, width, height, frame));
    EXPECT_EQ(frame, 12u); EXPECT_GT(pixels[2], 200u); EXPECT_LT(pixels[0], 80u);
    ASSERT_TRUE(player->seek_100ns(2'000'000));
    EXPECT_EQ(player->position_100ns(), 2'000'000);
    ASSERT_TRUE(player->copy_video_frame(pixels, width, height, frame));
    EXPECT_EQ(frame, 2u); EXPECT_GT(pixels[0], 200u); EXPECT_LT(pixels[2], 80u);
    ASSERT_EQ(player->run(), MediaStartResult::Started);
    EXPECT_GE(player->position_100ns(), 2'000'000);
    player->pause();
    const auto paused = player->position_100ns();
    std::this_thread::sleep_for(50ms); EXPECT_EQ(player->position_100ns(), paused);
    std::filesystem::remove_all(root);
}

TEST(MediaStartupTest, SlowSinkCommandsDoNotHoldFrameStateAndConstructionReturnsPending) {
    using namespace std::chrono_literals;
    using namespace vitreo::audio;
    char directory[] = "/tmp/vitreo_media_responsive_XXXXXX";
    ASSERT_NE(::mkdtemp(directory), nullptr);
    const std::filesystem::path root(directory);
    const auto path = root / "clip.mp4";
    const std::string command = "ffmpeg -y -hide_banner -loglevel error -f lavfi "
        "-i color=c=red:s=16x16:d=0.4:r=10 -f lavfi -i sine=frequency=440:duration=0.4 "
        "-pix_fmt yuv420p -shortest '" + path.string() + "'";
    ASSERT_EQ(std::system(command.c_str()), 0);
    start_succeeds.store(true); create_entered.store(false); create_delay_ms.store(2000);
    struct ResetDelay { ~ResetDelay() { create_delay_ms.store(0); } } reset;
    auto player = create_media_player(); ASSERT_NE(player, nullptr);
    ASSERT_TRUE(player->open(path.string()));
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!create_entered.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    ASSERT_TRUE(create_entered.load());
    auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(player->run(), MediaStartResult::Pending);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 100ms);
    player->pause();
    deadline = std::chrono::steady_clock::now() + 3s;
    while (!player->ready() && !player->failed() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    ASSERT_TRUE(player->ready());
    play_entered.store(false);
    MediaStartResult result = MediaStartResult::Failed;
    std::jthread runner([&] { result = player->run(); });
    deadline = std::chrono::steady_clock::now() + 1s;
    while (!play_entered.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    EXPECT_TRUE(play_entered.load());
    started = std::chrono::steady_clock::now();
    EXPECT_LT(player->position_100ns(), 1'000'000);
    std::vector<vitreo::u8> pixels; vitreo::u32 width = 0, height = 0; vitreo::u64 frame = 0;
    EXPECT_TRUE(player->copy_video_frame(pixels, width, height, frame));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 100ms);
    runner.join(); EXPECT_EQ(result, MediaStartResult::Started);
    std::filesystem::remove_all(root);
}

TEST(MediaStartupTest, AudioCursorCallbackDoesNotHoldThePipelineStateMutex) {
    using namespace std::chrono_literals;
    using namespace vitreo::audio;
    char directory[] = "/tmp/vitreo_media_cursor_XXXXXX";
    ASSERT_NE(::mkdtemp(directory), nullptr);
    const std::filesystem::path root(directory);
    const auto path = root / "clip.wav";
    const std::string command = "ffmpeg -y -hide_banner -loglevel error -f lavfi "
        "-i sine=frequency=440:duration=0.4 '" + path.string() + "'";
    ASSERT_EQ(std::system(command.c_str()), 0);
    start_succeeds.store(true);
    auto player = create_media_player(); ASSERT_NE(player, nullptr);
    ASSERT_TRUE(player->open(path.string()));
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!player->ready() && !player->failed() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    ASSERT_TRUE(player->ready()); ASSERT_EQ(player->run(), MediaStartResult::Started);
    cursor_delay_ms.store(500); cursor_entered.store(false);
    struct ResetCursor { ~ResetCursor() { cursor_delay_ms.store(0); } } reset;
    std::jthread query([&] { (void)player->position_100ns(); });
    const auto callback_deadline = std::chrono::steady_clock::now() + 1s;
    while (!cursor_entered.load() && std::chrono::steady_clock::now() < callback_deadline)
        std::this_thread::sleep_for(5ms);
    EXPECT_TRUE(cursor_entered.load());
    const auto started = std::chrono::steady_clock::now();
    EXPECT_GE(player->duration_100ns(), 3'000'000);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 100ms);
    query.join();
    std::filesystem::remove_all(root);
}
