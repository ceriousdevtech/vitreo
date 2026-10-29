/// @file test_capsule.cpp
/// @brief Unit tests for CLI-003: capsule commands

#include <gtest/gtest.h>
#include "app/cli/commands/capsule.hpp"
#include "app/cli/commands/command.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace vitreo::cli;

namespace fs = std::filesystem;

// =============================================================================
// Test Fixture
// =============================================================================

class CapsuleManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        char temp_template[] = "/tmp/vitreo_test_capsules_XXXXXX";
        char* dir = mkdtemp(temp_template);
        ASSERT_NE(dir, nullptr);
        test_dir_ = dir;
        manager_ = std::make_unique<CapsuleManager>(test_dir_);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    fs::path test_dir_;
    std::unique_ptr<CapsuleManager> manager_;
};

// =============================================================================
// CapsuleManager Tests
// =============================================================================

TEST_F(CapsuleManagerTest, CreateNewCapsuleSucceeds) {
    auto result = manager_->create("myapp");
    EXPECT_EQ(result.exit_code, 0);
}

TEST_F(CapsuleManagerTest, CreateCapsuleCreatesDirectories) {
    manager_->create("myapp");
    auto drive_path = test_dir_ / "myapp" / "drives" / "c";
    EXPECT_TRUE(fs::exists(drive_path));
    EXPECT_TRUE(fs::exists(test_dir_ / "myapp" / "registry"));
}

TEST_F(CapsuleManagerTest, RenameExistingCapsuleSucceeds) {
    ASSERT_EQ(manager_->create("source").exit_code, 0);
    EXPECT_EQ(manager_->rename("source", "destination").exit_code, 0);
    EXPECT_FALSE(manager_->exists("source"));
    EXPECT_TRUE(manager_->exists("destination"));
}

TEST_F(CapsuleManagerTest, CloneCopiesCapsuleContents) {
    ASSERT_EQ(manager_->create("source").exit_code, 0);
    const auto marker = manager_->capsule_path("source") / "drives" / "c" / "marker.txt";
    std::ofstream(marker) << "test";
    std::ofstream(
        manager_->capsule_path("source") / "runtime" / "processes" / "stale.json")
        << "{}";
    EXPECT_EQ(manager_->clone("source", "copy").exit_code, 0);
    EXPECT_TRUE(fs::exists(manager_->capsule_path("copy") / "drives" / "c" / "marker.txt"));
    EXPECT_FALSE(fs::exists(
        manager_->capsule_path("copy") / "runtime" / "processes" / "stale.json"));
}

TEST_F(CapsuleManagerTest, ExportAndImportRoundTripCapsule) {
    ASSERT_EQ(manager_->create("source").exit_code, 0);
    std::ofstream(
        manager_->capsule_path("source") / "drives" / "c" / "payload.txt")
        << "portable";
    const auto package = test_dir_.parent_path() /
        ("vitreo-export-" + std::to_string(::getpid()) + ".vitreocapsule");
    std::error_code ec;
    fs::remove_all(package, ec);

    ASSERT_EQ(manager_->export_to("source", package).exit_code, 0);
    ASSERT_EQ(manager_->import_from(package, "restored").exit_code, 0);
    EXPECT_TRUE(fs::exists(
        manager_->capsule_path("restored") / "drives" / "c" / "payload.txt"));

    fs::remove_all(package, ec);
}

TEST_F(CapsuleManagerTest, ImportRejectsInvalidDirectory) {
    const auto invalid = test_dir_ / "invalid-package";
    ASSERT_TRUE(fs::create_directories(invalid));
    EXPECT_NE(manager_->import_from(invalid, "restored").exit_code, 0);
}

TEST_F(CapsuleManagerTest, ImportRejectsSymbolicLinks) {
    const auto package = test_dir_ / "symlink-package";
    ASSERT_TRUE(fs::create_directories(package / "drives" / "c"));
    ASSERT_TRUE(fs::create_directories(package / "registry"));
    std::error_code ec;
    fs::create_symlink("/tmp", package / "drives" / "c" / "host", ec);
    ASSERT_FALSE(ec);

    const auto result = manager_->import_from(package, "restored");

    EXPECT_NE(result.exit_code, 0);
    EXPECT_FALSE(manager_->exists("restored"));
}

TEST_F(CapsuleManagerTest, ExportRejectsSymbolicLinks) {
    ASSERT_EQ(manager_->create("source").exit_code, 0);
    std::error_code ec;
    fs::create_symlink(
        "/tmp", manager_->capsule_path("source") / "drives" / "c" / "host", ec);
    ASSERT_FALSE(ec);
    const auto destination = test_dir_ / "unsafe-export.vitreocapsule";

    const auto result = manager_->export_to("source", destination);

    EXPECT_NE(result.exit_code, 0);
    EXPECT_FALSE(fs::exists(destination));
}

TEST_F(CapsuleManagerTest, ValidateReportsHealthyCapsule) {
    ASSERT_EQ(manager_->create("healthy").exit_code, 0);
    const auto result = manager_->validate("healthy", true);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.message.find("\"valid\":true"), std::string::npos);
}

TEST_F(CapsuleManagerTest, RejectsTraversalNames) {
    EXPECT_NE(manager_->create("../escape").exit_code, 0);
    EXPECT_NE(manager_->rename("safe", "../escape").exit_code, 0);
}

TEST_F(CapsuleManagerTest, CreateDuplicateFails) {
    manager_->create("myapp");
    auto result = manager_->create("myapp");
    EXPECT_NE(result.exit_code, 0);
}

TEST_F(CapsuleManagerTest, ListEmptyReturnsEmpty) {
    auto capsules = manager_->list();
    EXPECT_TRUE(capsules.empty());
}

TEST_F(CapsuleManagerTest, ListAfterCreateContainsCapsule) {
    manager_->create("myapp");
    auto capsules = manager_->list();
    ASSERT_EQ(capsules.size(), 1u);
    EXPECT_EQ(capsules[0].name, "myapp");
    EXPECT_TRUE(capsules[0].has_c_drive);
}

TEST_F(CapsuleManagerTest, DestroyExistingSucceeds) {
    manager_->create("myapp");
    auto result = manager_->destroy("myapp");
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_FALSE(fs::exists(test_dir_ / "myapp"));
}

TEST_F(CapsuleManagerTest, DestroyNonexistentFails) {
    auto result = manager_->destroy("ghost");
    EXPECT_NE(result.exit_code, 0);
}

TEST_F(CapsuleManagerTest, ExistsReturnsTrueAfterCreate) {
    manager_->create("myapp");
    EXPECT_TRUE(manager_->exists("myapp"));
}

TEST_F(CapsuleManagerTest, ExistsReturnsFalseBeforeCreate) {
    EXPECT_FALSE(manager_->exists("myapp"));
}

TEST_F(CapsuleManagerTest, CapsulePathCorrect) {
    auto expected = test_dir_ / "myapp";
    EXPECT_EQ(manager_->capsule_path("myapp"), expected);
}

// =============================================================================
// CapsuleCommand Tests
// =============================================================================

class CapsuleCommandTest : public CapsuleManagerTest {
protected:
    void SetUp() override {
        CapsuleManagerTest::SetUp();
        cmd_ = std::make_unique<CapsuleCommand>(*manager_);
    }

    ParsedArgs make_args(const std::string& sub,
                         std::vector<std::string> positional = {}) {
        ParsedArgs args;
        args.command = "capsule";
        args.subcommand = sub;
        args.positional_args = std::move(positional);
        return args;
    }

    std::unique_ptr<CapsuleCommand> cmd_;
};

TEST_F(CapsuleCommandTest, CreateSubcommandSucceeds) {
    auto args = make_args("create", {"testcap"});
    auto result = cmd_->execute(args);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_TRUE(manager_->exists("testcap"));
}

TEST_F(CapsuleCommandTest, ListSubcommandSucceeds) {
    manager_->create("cap1");
    auto args = make_args("list");
    auto result = cmd_->execute(args);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.message.find("cap1"), std::string::npos);
}

TEST_F(CapsuleCommandTest, DestroySubcommandSucceeds) {
    manager_->create("cap1");
    auto args = make_args("destroy", {"cap1"});
    auto result = cmd_->execute(args);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_FALSE(manager_->exists("cap1"));
}

TEST_F(CapsuleCommandTest, UnknownSubcommandFails) {
    auto args = make_args("frobnicate");
    auto result = cmd_->execute(args);
    EXPECT_NE(result.exit_code, 0);
}
