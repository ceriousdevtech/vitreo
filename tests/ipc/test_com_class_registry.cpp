// SPDX-License-Identifier: LGPL-2.1-only

#include "ipc/com_class_registry.hpp"

#include <gtest/gtest.h>

#include <filesystem>

#include <unistd.h>

namespace vitreo::ipc {
namespace {

class ComClassRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_root = std::filesystem::temp_directory_path() /
            ("vitreo-com-class-" + std::to_string(::getpid()));
        std::error_code ec;
        std::filesystem::remove_all(m_root, ec);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(m_root, ec);
    }

    std::filesystem::path m_root;
};

TEST_F(ComClassRegistryTest, PublishesSuspendsResumesAndRevokesClassObject) {
    const ComClassId clsid{
        0x12, 0x34, 0x56, 0x78, 0x90, 0xab, 0xcd, 0xef,
        0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
    constexpr u32 kLocalServer = 0x4;
    constexpr u64 kClassObject = 0x1234567887654321ull;
    ComClassSnapshot found{};

    {
        ComClassRegistration registration(
            m_root.string(), clsid, 7, kClassObject, kLocalServer, 1, 4,
            true);
        ASSERT_TRUE(registration.valid());
        EXPECT_FALSE(find_com_class(
            m_root.string(), clsid, kLocalServer, found));

        ASSERT_TRUE(registration.set_suspended(false));
        ASSERT_TRUE(find_com_class(
            m_root.string(), clsid, kLocalServer, found));
        EXPECT_EQ(found.owner_pid, static_cast<u32>(::getpid()));
        EXPECT_EQ(found.cookie, 7u);
        EXPECT_EQ(found.class_object, kClassObject);
        EXPECT_EQ(found.pointer_size, 4u);
        EXPECT_FALSE(find_com_class(m_root.string(), clsid, 0x1, found));

        ASSERT_TRUE(registration.set_suspended(true));
        EXPECT_FALSE(find_com_class(
            m_root.string(), clsid, kLocalServer, found));
    }

    EXPECT_FALSE(find_com_class(
        m_root.string(), clsid, kLocalServer, found));
}

TEST_F(ComClassRegistryTest, PublishesAndRevokesRunningObjectByMoniker) {
    constexpr std::string_view kMoniker =
        "!{8B8F45A0-C312-984D-8183-BFF84F2770D2}";
    constexpr u64 kObject = 0x1234567887654321ull;
    RotObjectSnapshot found{};

    {
        RotObjectRegistration registration(
            m_root.string(), kMoniker, 17, kObject, 8);
        ASSERT_TRUE(registration.valid());
        ASSERT_TRUE(find_rot_object(m_root.string(), kMoniker, found));
        EXPECT_EQ(found.owner_pid, static_cast<u32>(::getpid()));
        EXPECT_EQ(found.cookie, 17u);
        EXPECT_EQ(found.object, kObject);
        EXPECT_EQ(found.pointer_size, 8u);
        EXPECT_FALSE(find_rot_object(
            m_root.string(), "!{00000000-0000-0000-0000-000000000000}",
            found));
    }

    EXPECT_FALSE(find_rot_object(m_root.string(), kMoniker, found));
}

} // namespace
} // namespace vitreo::ipc
