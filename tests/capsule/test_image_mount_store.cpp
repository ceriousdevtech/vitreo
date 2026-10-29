#include <gtest/gtest.h>

#include "capsule/vfs/image_mount_store.hpp"

#include <filesystem>

using namespace vitreo::capsule;

class ImageMountStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        char pattern[] = "/tmp/vitreo_mount_store_XXXXXX";
        char* directory = ::mkdtemp(pattern);
        ASSERT_NE(directory, nullptr);
        root_ = directory;
    }

    void TearDown() override {
        std::filesystem::remove_all(root_);
    }

    std::filesystem::path root_;
};

TEST_F(ImageMountStoreTest, PersistsListsAndRemovesMount) {
    ImageMountStore store(root_);
    ASSERT_TRUE(store.save('D', "/images/game.iso").is_ok());
    auto records = store.list();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].drive, 'D');
    EXPECT_EQ(records[0].image_path, "/images/game.iso");

    ASSERT_TRUE(store.remove('d').is_ok());
    EXPECT_TRUE(store.list().empty());
}

TEST_F(ImageMountStoreTest, ChoosesNextLetterAfterDirectoriesAndImageMounts) {
    std::filesystem::create_directories(root_ / "drives" / "d");
    ImageMountStore store(root_);
    ASSERT_TRUE(store.save('E', "/images/first.iso").is_ok());
    EXPECT_EQ(store.next_available_drive(), 'F');
}

TEST_F(ImageMountStoreTest, RejectsSystemDrive) {
    ImageMountStore store(root_);
    auto saved = store.save('C', "/images/game.iso");
    ASSERT_TRUE(saved.is_err());
    EXPECT_EQ(saved.error(), VfsError::InvalidArgument);
}
