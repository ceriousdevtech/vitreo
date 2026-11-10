/// @file test_cd_image.cpp
/// @brief CdImage: table of contents from the Q subchannel, frame mapping,
///        and IsoFileSystem on a mixed-mode image.

#include <gtest/gtest.h>

#include "cd_image_fixtures.hpp"
#include "capsule/vfs/cd_image.hpp"
#include "capsule/vfs/iso_filesystem.hpp"

using namespace vitreo;
using namespace vitreo::capsule;

TEST(CdImageTest, QDecodeChecksTheCrc) {
    const auto good = test_cd::q_block(4, 1, 1, 10, 160);
    std::array<u8, 12> q{};
    EXPECT_TRUE(CdImage::decode_q(good, q));
    EXPECT_EQ(q[0], 0x41);
    EXPECT_EQ(q[1], 0x01);
    EXPECT_EQ(q[9], 0x10);  // abs frame 160 = 00:02:10
    const auto bad = test_cd::q_block(4, 1, 1, 10, 160, /*corrupt=*/true);
    EXPECT_FALSE(CdImage::decode_q(bad, q));
}

TEST(CdImageTest, IsoImageIsOneDataTrack) {
    const auto path = test_cd::make_iso("vitreo_cd_one_track.iso", 1491);
    std::string error;
    auto image = CdImage::open(path.string(), error);
    ASSERT_NE(image, nullptr) << error;
    ASSERT_EQ(image->tracks().size(), 1u);
    EXPECT_EQ(image->tracks()[0].number, 1);
    EXPECT_FALSE(image->tracks()[0].is_audio());
    EXPECT_EQ(image->tracks()[0].start_lba, 0u);
    EXPECT_EQ(image->tracks()[0].length, 1491u);
    EXPECT_EQ(image->leadout_lba(), 1491u);
    EXPECT_FALSE(image->has_raw_frames());
    std::array<u8, 2352> raw{};
    EXPECT_FALSE(image->read_raw(0, raw));  // no frames behind an ISO
    std::filesystem::remove(path);
}

TEST(CdImageTest, SubchannelTocPlacesTracksByTheirAbsoluteTime) {
    test_cd::MixedLayout layout;
    const auto path = test_cd::make_mixed_mode_mdf("vitreo_cd_mixed.mdf", layout);
    std::string error;
    auto image = CdImage::open(path.string(), error);
    ASSERT_NE(image, nullptr) << error;
    ASSERT_EQ(image->tracks().size(), 2u);
    const u32 audio_start = layout.data_frames + 1 + layout.omitted;
    const CdTrack& data = image->tracks()[0];
    const CdTrack& audio = image->tracks()[1];
    EXPECT_EQ(data.number, 1);
    EXPECT_FALSE(data.is_audio());
    EXPECT_EQ(data.start_lba, 0u);
    EXPECT_EQ(data.length, audio_start);
    EXPECT_EQ(audio.number, 2);
    EXPECT_TRUE(audio.is_audio());
    EXPECT_EQ(audio.start_lba, audio_start);
    EXPECT_EQ(audio.length, layout.audio_frames);
    EXPECT_EQ(image->leadout_lba(), audio_start + layout.audio_frames);

    // Frames come from their address, not their position in the file; the
    // omitted pregap reads as silence.
    std::array<u8, 2352> raw{};
    ASSERT_TRUE(image->read_raw(audio_start + 5, raw));
    EXPECT_EQ(raw[0] | (raw[1] << 8), test_cd::audio_sample(5, 0));
    EXPECT_EQ(raw[2] | (raw[3] << 8), test_cd::audio_sample(5, 1));
    ASSERT_TRUE(image->read_raw(layout.data_frames + 3, raw));
    EXPECT_TRUE(std::all_of(raw.begin(), raw.end(), [](u8 b) { return b == 0; }));
    EXPECT_FALSE(image->read_raw(image->leadout_lba(), raw));

    std::array<u8, 2048> user{};
    ASSERT_TRUE(image->read_user_data(16, user));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(user.data()) + 1, 5), "CD001");
    std::filesystem::remove(path);
}

TEST(CdImageTest, IsoFileSystemMountsTrackOneOfAMixedModeImage) {
    test_cd::MixedLayout layout;
    const auto path = test_cd::make_mixed_mode_mdf("vitreo_cd_mixed_fs.mdf", layout, "ARMADA_TEST");
    IsoFileSystem fs(path.string());
    ASSERT_TRUE(fs.valid()) << fs.error_message();
    VolumeInfo info{};
    ASSERT_TRUE(fs.volume_info(info));
    EXPECT_EQ(info.label, "ARMADA_TEST");
    EXPECT_EQ(info.total_clusters, layout.data_frames);
    ASSERT_NE(fs.cd_image(), nullptr);
    EXPECT_EQ(fs.cd_image()->tracks().size(), 2u);

    // The same volume from the 2048-byte ISO of the same data: same serial.
    const auto iso = test_cd::make_iso("vitreo_cd_mixed_fs.iso", layout.data_frames, "ARMADA_TEST");
    IsoFileSystem iso_fs(iso.string());
    ASSERT_TRUE(iso_fs.valid());
    VolumeInfo iso_info{};
    ASSERT_TRUE(iso_fs.volume_info(iso_info));
    EXPECT_EQ(iso_info.label, info.label);
    std::array<u8, 2048> pvd{}, iso_pvd{};
    ASSERT_TRUE(fs.read_volume(16 * 2048, pvd).is_ok());
    ASSERT_TRUE(iso_fs.read_volume(16 * 2048, iso_pvd).is_ok());
    EXPECT_EQ(cdfs_volume_serial(pvd), cdfs_volume_serial(iso_pvd));
    EXPECT_EQ(pvd, iso_pvd);
    std::filesystem::remove(path);
    std::filesystem::remove(iso);
}
