// SPDX-License-Identifier: LGPL-2.1-only
//
// The guest command line that is known BEFORE a run creates any state.
//
// VITREO_SCOPED_ENV is matched against the guest command line, and it must be
// delivered before the runtime image's static initialisers run, i.e. from
// main(), long before execute_pe. main() therefore asks known_guest_command_line
// for the line, and execute_pe must give the guest the very same one: if the
// two ever disagreed, a process would be re-executed with variables its real
// command line does not select (or miss ones it does).

#include "app/cli/commands/run.hpp"
#include "app/cli/parser/arg_parser.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#include <unistd.h>

namespace vitreo::cli {
namespace {

namespace fs = std::filesystem;

class GuestCommandLineTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (const char* home = std::getenv("HOME")) m_old_home = home;
        m_root = fs::temp_directory_path() /
                 ("vitreo-guest-cmdline-test-" + std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(m_root, ec);
        fs::create_directories(capsule_root() / "drives" / "c" / "Apps", ec);
        ASSERT_FALSE(ec);
        ::setenv("HOME", m_root.c_str(), 1);
    }

    void TearDown() override {
        if (m_old_home) {
            ::setenv("HOME", m_old_home->c_str(), 1);
        } else {
            ::unsetenv("HOME");
        }
        std::error_code ec;
        fs::remove_all(m_root, ec);
    }

    [[nodiscard]] fs::path capsule_root() const {
        return m_root / ".vitreo" / "capsules" / "cap";
    }

    [[nodiscard]] RunConfig config_for(const ParsedArgs& args) const {
        RunConfig config;
        assign_launch_identity(config, args);
        return config;
    }

    static ParsedArgs run_args(std::vector<std::string> positional) {
        ParsedArgs args;
        args.command = "run";
        args.positional_args = std::move(positional);
        args.capsule_name = "cap";
        return args;
    }

private:
    fs::path m_root;
    std::optional<std::string> m_old_home;
};

TEST_F(GuestCommandLineTest, ALauncherSuppliedCommandLineIsUsedVerbatim) {
    ParsedArgs args = run_args({"/tmp/snapshot.exe"});
    args.relaunched_as = "C:\\Program Files\\App\\host.exe";
    args.guest_cmdline =
        "\"C:\\Program Files\\App\\host.exe\" --type=renderer --lang=en-US";
    EXPECT_EQ(known_guest_command_line(config_for(args)), args.guest_cmdline);
}

TEST_F(GuestCommandLineTest, ARelaunchedChildWithoutOneIsItsInstallPathAndArguments) {
    ParsedArgs args = run_args({"/tmp/snapshot.exe", "--flag", "two words"});
    args.relaunched_as = "C:\\Program Files\\App\\host.exe";
    EXPECT_EQ(known_guest_command_line(config_for(args)),
              std::string("\"C:\\Program Files\\App\\host.exe\" --flag \"two words\""));
}

TEST_F(GuestCommandLineTest, AWindowsPathInsideTheCapsuleIsTheCommandLine) {
    const ParsedArgs args = run_args({"C:\\Apps\\tool.exe", "/S"});
    EXPECT_EQ(known_guest_command_line(config_for(args)),
              std::string("\"C:\\Apps\\tool.exe\" /S"));
}

TEST_F(GuestCommandLineTest, AHostPathInsideTheCapsuleIsItsWindowsPath) {
    const std::string host =
        (capsule_root() / "drives" / "c" / "Apps" / "tool.exe").string();
    const ParsedArgs args = run_args({host});
    EXPECT_EQ(known_guest_command_line(config_for(args)),
              std::string("\"C:\\Apps\\tool.exe\""));
}

TEST_F(GuestCommandLineTest, AnExecutableOutsideTheCapsuleHasNoCommandLineYet) {
    // Its guest directory is chosen when the directory is published into the
    // capsule, inside execute_pe: nothing can match it earlier.
    const ParsedArgs args = run_args({"/usr/local/bin/outside.exe"});
    EXPECT_EQ(known_guest_command_line(config_for(args)), std::nullopt);
}

TEST_F(GuestCommandLineTest, TheLaunchIdentityComesFromTheParsedArguments) {
    ParsedArgs args = run_args({"C:\\Apps\\tool.exe", "--windowed"});
    args.relaunched_as = "C:\\Apps\\tool.exe";
    args.guest_cmdline = "\"C:\\Apps\\tool.exe\" --windowed";
    const RunConfig config = config_for(args);
    EXPECT_EQ(config.pe_path, "C:\\Apps\\tool.exe");
    EXPECT_EQ(config.capsule_name, "cap");
    EXPECT_EQ(config.relaunched_as, "C:\\Apps\\tool.exe");
    EXPECT_EQ(config.guest_cmdline, "\"C:\\Apps\\tool.exe\" --windowed");
    EXPECT_EQ(config.guest_args, (std::vector<std::string>{"--windowed"}));
}

}  // namespace
}  // namespace vitreo::cli
