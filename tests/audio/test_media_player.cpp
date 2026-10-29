#include "audio/contracts/media_player.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using vitreo::audio::create_media_player;
using vitreo::u32;
using vitreo::u64;

TEST(MediaPlayerTest, CopyVideoFrameFromDecodedColorSource) {
    auto player = create_media_player();
    if (player == nullptr) {
        GTEST_SKIP() << "no host media decoder";
    }

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "vitreo_media_red.mp4";
    const std::string command =
        "ffmpeg -y -hide_banner -loglevel error -f lavfi "
        "-i color=c=red:s=16x16:d=0.4:r=10 -an -pix_fmt yuv420p \"" +
        path.string() + "\"";
    if (std::system(command.c_str()) != 0) {
        GTEST_SKIP() << "ffmpeg could not write a test video";
    }

    ASSERT_TRUE(player->open(path.string()));
    EXPECT_TRUE(player->has_video());
    // IMediaPosition::get_Duration is available after RenderFile, from the
    // container — not after the async PCM worker finishes.
    EXPECT_GE(player->duration_100ns(), 3'000'000);
    EXPECT_LE(player->duration_100ns(), 6'000'000);

    std::vector<vitreo::u8> rgba;
    u32 width = 0;
    u32 height = 0;
    u64 frame_index = 0;
    ASSERT_TRUE(player->copy_video_frame(rgba, width, height, frame_index))
        << "open() must preroll the first video picture";
    EXPECT_EQ(width, 16u);
    EXPECT_EQ(height, 16u);
    ASSERT_EQ(rgba.size(), 16u * 16u * 4u);
    EXPECT_GT(rgba[0], 200u);
    EXPECT_LT(rgba[1], 80u);
    EXPECT_LT(rgba[2], 80u);
}
