/// @file test_cli.cpp
/// @brief Unit tests for CLI-001: arg_parser + command dispatcher

#include <gtest/gtest.h>
#include "app/cli/parser/arg_parser.hpp"
#include "app/cli/commands/command.hpp"
#include "app/cli/commands/installer_packages.hpp"
#include <memory>
#include <string>
#include <vector>

using namespace vitreo::cli;

// =============================================================================
// Helper
// =============================================================================

struct ArgBuilder {
    std::vector<std::string> args;
    ArgBuilder() { args.push_back("vitreoctl"); } // argv[0]
    ArgBuilder& add(std::string a) { args.push_back(std::move(a)); return *this; }
    std::vector<const char*> build() const {
        std::vector<const char*> ptrs;
        for (const auto& a : args) ptrs.push_back(a.c_str());
        return ptrs;
    }
};

// =============================================================================
// parse_args Tests
// =============================================================================

TEST(ArgParserTest, NoArgsReturnsError) {
    ArgBuilder ab;
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
}

TEST(ArgParserTest, SingleCommandRun) {
    ArgBuilder ab;
    ab.add("run");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.args.command, "run");
}

TEST(ArgParserTest, CapsuleCreateSubcommand) {
    ArgBuilder ab;
    ab.add("capsule").add("create");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.args.command, "capsule");
    EXPECT_EQ(result.args.subcommand, "create");
}

TEST(ArgParserTest, VerboseFlagBeforeCommand) {
    ArgBuilder ab;
    ab.add("--verbose").add("run");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(result.success);
    EXPECT_TRUE(result.args.verbose);
    EXPECT_EQ(result.args.command, "run");
}

TEST(ArgParserTest, HelpFlagAloneBecomesHelpCommand) {
    ArgBuilder ab;
    ab.add("--help");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.args.command, "help");
}

TEST(ArgParserTest, ShortVerboseAndHelp) {
    ArgBuilder ab;
    ab.add("-v").add("-h");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(result.success);
    EXPECT_TRUE(result.args.verbose);
    EXPECT_TRUE(result.args.help);
}

TEST(ArgParserTest, CapsuleNamedOption) {
    ArgBuilder ab;
    ab.add("--capsule").add("myapp").add("run");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(result.success);
    ASSERT_TRUE(result.args.capsule_name.has_value());
    EXPECT_EQ(*result.args.capsule_name, "myapp");
    EXPECT_EQ(result.args.command, "run");
}

TEST(ArgParserTest, CapsuleMissingValueReturnsError) {
    ArgBuilder ab;
    ab.add("--capsule");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
}

TEST(ArgParserTest, AppNameOverrideIsParsed) {
    ArgBuilder ab;
    ab.add("run")
        .add("C:\\Games\\launcher.exe")
        .add("--app-name")
        .add("My Game");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(result.success);
    ASSERT_TRUE(result.args.app_name.has_value());
    EXPECT_EQ(*result.args.app_name, "My Game");
}

TEST(ArgParserTest, AppNameMissingValueReturnsError) {
    ArgBuilder ab;
    ab.add("run").add("game.exe").add("--app-name");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error.find("--app-name"), std::string::npos);
}

TEST(ArgParserTest, MountDriveOption) {
    ArgBuilder ab;
    ab.add("mount").add("disc.iso").add("--drive").add("E");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.args.command, "mount");
    ASSERT_TRUE(result.args.drive_letter.has_value());
    EXPECT_EQ(*result.args.drive_letter, "E");
}

TEST(ArgParserTest, DriveMissingValueReturnsError) {
    ArgBuilder ab;
    ab.add("mount").add("disc.iso").add("--drive");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    EXPECT_FALSE(result.success);
}

TEST(ArgParserTest, UnknownFlagReturnsError) {
    ArgBuilder ab;
    ab.add("--bogus");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error.find("--bogus"), std::string::npos);
}

TEST(ArgParserTest, DoubleDashCollectsPositionalArgs) {
    ArgBuilder ab;
    ab.add("run").add("--").add("extra").add("args");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.args.command, "run");
    ASSERT_EQ(result.args.positional_args.size(), 2u);
    EXPECT_EQ(result.args.positional_args[0], "extra");
    EXPECT_EQ(result.args.positional_args[1], "args");
}

TEST(ArgParserTest, CapsuleListWithPositionalArg) {
    ArgBuilder ab;
    ab.add("capsule").add("list").add("arg1");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.args.command, "capsule");
    EXPECT_EQ(result.args.subcommand, "list");
    ASSERT_EQ(result.args.positional_args.size(), 1u);
    EXPECT_EQ(result.args.positional_args[0], "arg1");
}

TEST(ArgParserTest, VersionCommandParsed) {
    ArgBuilder ab;
    ab.add("version");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.args.command, "version");
}

TEST(ArgParserTest, VerboseAfterCommand) {
    ArgBuilder ab;
    ab.add("--verbose").add("capsule").add("create");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(result.success);
    EXPECT_TRUE(result.args.verbose);
    EXPECT_EQ(result.args.command, "capsule");
    EXPECT_EQ(result.args.subcommand, "create");
}

TEST(ArgParserTest, ConfigExpectsSubcommand) {
    ArgBuilder ab;
    ab.add("config").add("get");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.args.command, "config");
    EXPECT_EQ(result.args.subcommand, "get");
}

TEST(ArgParserTest, ProcessExpectsSubcommandAndJsonFlag) {
    ArgBuilder ab;
    ab.add("process").add("list").add("--capsule").add("games").add("--json");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.args.command, "process");
    EXPECT_EQ(result.args.subcommand, "list");
    EXPECT_TRUE(result.args.json);
    ASSERT_TRUE(result.args.capsule_name.has_value());
    EXPECT_EQ(*result.args.capsule_name, "games");
}

TEST(ArgParserTest, GuestEnvironmentOverridesAreRepeatable) {
    ArgBuilder ab;
    ab.add("run").add("app.exe")
        .add("--env").add("MODE=safe")
        .add("--env").add("CACHE=C:\\Cache")
        .add("--env").add("=C:=C:\\Work");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.args.guest_environment.size(), 3u);
    EXPECT_EQ(result.args.guest_environment[0], "MODE=safe");
    EXPECT_EQ(result.args.guest_environment[1], "CACHE=C:\\Cache");
    EXPECT_EQ(result.args.guest_environment[2], "=C:=C:\\Work");
}

TEST(ArgParserTest, GuestEnvironmentRequiresNameAndValueSeparator) {
    ArgBuilder ab;
    ab.add("run").add("app.exe").add("--env").add("INVALID");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    EXPECT_FALSE(result.success);
}

TEST(ArgParserTest, CapsuleNameDefaultEmpty) {
    ArgBuilder ab;
    ab.add("run");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(result.success);
    EXPECT_FALSE(result.args.capsule_name.has_value());
}

TEST(ArgParserTest, SupervisedWorkerContextIsParsed) {
    ArgBuilder ab;
    ab.add("run")
        .add("--relaunched-as")
        .add("C:\\Program Files (x86)\\Starcraft\\StarEdit.exe")
        .add("--guest-cwd")
        .add("C:\\Program Files (x86)\\Starcraft")
        .add("--guest-elevation-credential")
        .add("0011aaff")
        .add("--guest-token-context-fd")
        .add("17")
        .add("--guest-service-arg-utf16")
        .add("002D002D0063006F006D")
        .add("--guest-parent-pid")
        .add("4242")
        .add("--capsule")
        .add("Starcraft")
        .add("/tmp/vitreo_launch_child.exe");
    auto argv = ab.build();
    auto result = parse_args(static_cast<int>(argv.size()), argv.data());
    ASSERT_TRUE(result.success);
    ASSERT_TRUE(result.args.relaunched_as.has_value());
    EXPECT_EQ(*result.args.relaunched_as,
              "C:\\Program Files (x86)\\Starcraft\\StarEdit.exe");
    ASSERT_TRUE(result.args.guest_cwd.has_value());
    EXPECT_EQ(*result.args.guest_cwd,
              "C:\\Program Files (x86)\\Starcraft");
    ASSERT_TRUE(result.args.capsule_name.has_value());
    EXPECT_EQ(*result.args.capsule_name, "Starcraft");
    ASSERT_TRUE(result.args.guest_elevation_credential.has_value());
    EXPECT_EQ(*result.args.guest_elevation_credential, "0011aaff");
    ASSERT_TRUE(result.args.guest_token_context_fd.has_value());
    EXPECT_EQ(*result.args.guest_token_context_fd, "17");
    ASSERT_EQ(result.args.guest_service_arguments_utf16.size(), 1u);
    EXPECT_EQ(result.args.guest_service_arguments_utf16[0],
              "002D002D0063006F006D");
    ASSERT_TRUE(result.args.guest_parent_pid.has_value());
    EXPECT_EQ(*result.args.guest_parent_pid, "4242");
    ASSERT_EQ(result.args.positional_args.size(), 1u);
    EXPECT_EQ(result.args.positional_args[0],
              "/tmp/vitreo_launch_child.exe");
}

// =============================================================================
// CommandDispatcher Tests
// =============================================================================

TEST(CommandDispatcherTest, NoCommandsDispatchFails) {
    CommandDispatcher disp;
    ParsedArgs args;
    args.command = "anything";
    auto result = disp.dispatch(args);
    EXPECT_NE(result.exit_code, 0);
}

TEST(InstallerPackageTest, ClassifiesWindowsInstallerTypes) {
    EXPECT_EQ(classify_installer_package("/tmp/setup.msi").action,
              InstallerPackageAction::Install);
    EXPECT_EQ(classify_installer_package("C:\\Patches\\update.MSP").action,
              InstallerPackageAction::Patch);
    EXPECT_EQ(classify_installer_package("setup.MST").action,
              InstallerPackageAction::Unsupported);
    EXPECT_EQ(classify_installer_package("module.msm").action,
              InstallerPackageAction::Unsupported);
    EXPECT_EQ(classify_installer_package("app.msix").action,
              InstallerPackageAction::Unsupported);
    EXPECT_EQ(classify_installer_package("app.appxbundle").action,
              InstallerPackageAction::Unsupported);
    EXPECT_EQ(classify_installer_package("update.msu").action,
              InstallerPackageAction::Unsupported);
    EXPECT_EQ(classify_installer_package("program.exe").action,
              InstallerPackageAction::None);
    EXPECT_NE(std::string(classify_installer_package("app.msix").detail)
                  .find("UNIMPLEMENTED"),
              std::string::npos);
}

TEST(CommandDispatcherTest, DispatchVersion) {
    CommandDispatcher disp;
    disp.register_command(std::make_unique<VersionCommand>());
    ParsedArgs args;
    args.command = "version";
    auto result = disp.dispatch(args);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.message.find("1.0.0-beta.1"), std::string::npos);
}

TEST(CommandDispatcherTest, HasCommandReturnsTrue) {
    CommandDispatcher disp;
    disp.register_command(std::make_unique<VersionCommand>());
    EXPECT_TRUE(disp.has_command("version"));
}

TEST(CommandDispatcherTest, HasCommandReturnsFalse) {
    CommandDispatcher disp;
    EXPECT_FALSE(disp.has_command("nonexistent"));
}

TEST(CommandDispatcherTest, DispatchEmptyCommandFails) {
    CommandDispatcher disp;
    ParsedArgs args;
    auto result = disp.dispatch(args);
    EXPECT_NE(result.exit_code, 0);
}

// =============================================================================
// VersionCommand Tests
// =============================================================================

TEST(VersionCommandTest, ExecuteReturnsZero) {
    VersionCommand cmd;
    ParsedArgs args;
    auto result = cmd.execute(args);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.message.find("Vitreo"), std::string::npos);
}

TEST(VersionCommandTest, NameIsVersion) {
    VersionCommand cmd;
    EXPECT_EQ(cmd.name(), "version");
}

TEST(VersionCommandTest, HelpNotEmpty) {
    VersionCommand cmd;
    EXPECT_FALSE(cmd.help().empty());
}
