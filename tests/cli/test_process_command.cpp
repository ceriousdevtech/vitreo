// SPDX-License-Identifier: LGPL-2.1-only

#include "app/cli/commands/process.hpp"
#include "ipc/process_registry.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include <unistd.h>

namespace vitreo::cli {
namespace {

class ProcessCommandTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
            ("vitreo-process-command-" + std::to_string(::getpid()));
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(root_ / "test");
        command_ = std::make_unique<ProcessCommand>(root_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    std::filesystem::path root_;
    std::unique_ptr<ProcessCommand> command_;
};

TEST_F(ProcessCommandTest, ListsRegisteredProcessAsJson) {
    ipc::ProcessRegistration registration(
        (root_ / "test").string(), "tool.exe", "C:\\Tools\\tool.exe");
    ASSERT_TRUE(registration.valid());

    ParsedArgs args;
    args.command = "process";
    args.subcommand = "list";
    args.capsule_name = "test";
    args.json = true;
    const auto result = command_->execute(args);

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.message.find("\"image_name\":\"tool.exe\""),
              std::string::npos);
    EXPECT_NE(result.message.find("\"capsule\":\"test\""),
              std::string::npos);
}

TEST_F(ProcessCommandTest, RefusesToSignalUnregisteredPid) {
    ParsedArgs args;
    args.command = "process";
    args.subcommand = "stop";
    args.capsule_name = "test";
    args.positional_args = {"999999"};
    const auto result = command_->execute(args);
    EXPECT_NE(result.exit_code, 0);
}

} // namespace
} // namespace vitreo::cli
