/// @file test_config.cpp
/// @brief Unit tests for CLI-004: config command

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include "app/cli/commands/config.hpp"

using namespace vitreo;
using namespace vitreo::cli;

// =============================================================================
// Config data-model tests
// =============================================================================

TEST(ConfigTest, DefaultCpuModeIsInterpreter) {
    Config cfg;
    auto val = cfg.get("cpu.mode");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "interpreter");
}

TEST(ConfigTest, SetAndGetRoundTrip) {
    Config cfg;
    EXPECT_TRUE(cfg.set("log.file", "/tmp/vitreo.log"));
    auto val = cfg.get("log.file");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "/tmp/vitreo.log");
}

TEST(ConfigTest, SetInvalidKeyReturnsFalse) {
    Config cfg;
    EXPECT_FALSE(cfg.set("nonexistent.key", "value"));
}

TEST(ConfigTest, SetCpuModeToJitSucceeds) {
    Config cfg;
    EXPECT_TRUE(cfg.set("cpu.mode", "jit"));
    EXPECT_EQ(*cfg.get("cpu.mode"), "jit");
}

TEST(ConfigTest, SetCpuModeToInvalidFails) {
    Config cfg;
    EXPECT_FALSE(cfg.set("cpu.mode", "invalid_mode"));
    // Value should remain unchanged
    EXPECT_EQ(*cfg.get("cpu.mode"), "interpreter");
}

TEST(ConfigTest, SetLogLevelAllValidValues) {
    Config cfg;
    for (const char* level : {"error", "warn", "info", "debug", "trace"}) {
        EXPECT_TRUE(cfg.set("log.level", level))
            << "Expected '" << level << "' to be accepted";
    }
}

TEST(ConfigTest, SetLogLevelInvalidFails) {
    Config cfg;
    EXPECT_FALSE(cfg.set("log.level", "invalid"));
    EXPECT_EQ(*cfg.get("log.level"), "warn");
}

TEST(ConfigTest, ResetRestoresDefault) {
    Config cfg;
    cfg.set("cpu.mode", "jit");
    EXPECT_EQ(*cfg.get("cpu.mode"), "jit");

    EXPECT_TRUE(cfg.reset("cpu.mode"));
    EXPECT_EQ(*cfg.get("cpu.mode"), "interpreter");
}

TEST(ConfigTest, ResetAllRestoresAllDefaults) {
    Config cfg;
    cfg.set("cpu.mode", "jit");
    cfg.set("log.level", "debug");
    cfg.set("memory.guest_size_mb", "2048");

    cfg.reset_all();

    EXPECT_EQ(*cfg.get("cpu.mode"), "interpreter");
    EXPECT_EQ(*cfg.get("log.level"), "warn");
    EXPECT_EQ(*cfg.get("memory.guest_size_mb"), "4096");
}

TEST(ConfigTest, GetAllReturnsEightKeys) {
    Config cfg;
    auto all = cfg.get_all();
    EXPECT_EQ(all.size(), 8u);
}

TEST(ConfigTest, IsValidKeyReturnsTrueForKnown) {
    Config cfg;
    EXPECT_TRUE(cfg.is_valid_key("cpu.mode"));
    EXPECT_TRUE(cfg.is_valid_key("log.level"));
    EXPECT_TRUE(cfg.is_valid_key("capsule.auto_create"));
}

TEST(ConfigTest, IsValidKeyReturnsFalseForUnknown) {
    Config cfg;
    EXPECT_FALSE(cfg.is_valid_key("does.not.exist"));
    EXPECT_FALSE(cfg.is_valid_key(""));
}

TEST(ConfigTest, NumericKeyRejectsNonNumeric) {
    Config cfg;
    EXPECT_FALSE(cfg.set("cpu.max_instructions", "abc"));
    EXPECT_FALSE(cfg.set("memory.guest_size_mb", "-1"));
    EXPECT_FALSE(cfg.set("memory.stack_size_mb", "3.14"));
}

TEST(ConfigTest, NumericKeyAcceptsValidUint) {
    Config cfg;
    EXPECT_TRUE(cfg.set("cpu.max_instructions", "1000000"));
    EXPECT_EQ(*cfg.get("cpu.max_instructions"), "1000000");
}

TEST(ConfigTest, CapsuleAutoCreateValidation) {
    Config cfg;
    EXPECT_TRUE(cfg.set("capsule.auto_create", "false"));
    EXPECT_TRUE(cfg.set("capsule.auto_create", "true"));
    EXPECT_FALSE(cfg.set("capsule.auto_create", "yes"));
}

TEST(ConfigTest, ResetUnknownKeyReturnsFalse) {
    Config cfg;
    EXPECT_FALSE(cfg.reset("nonexistent.key"));
}

TEST(ConfigTest, GetUnknownKeyReturnsNullopt) {
    Config cfg;
    EXPECT_FALSE(cfg.get("nonexistent.key").has_value());
}

// =============================================================================
// ConfigCommand tests
// =============================================================================

namespace {

/// Helper to build ParsedArgs for config commands
ParsedArgs make_config_args(const std::string& subcommand,
                            std::vector<std::string> positional = {},
                            bool all_flag = false) {
    ParsedArgs args;
    args.command = "config";
    args.subcommand = subcommand;
    args.positional_args = std::move(positional);
    args.all = all_flag;
    return args;
}

} // anonymous namespace

TEST(ConfigCommandTest, GetAllReturnsSuccess) {
    Config cfg;
    ConfigCommand cmd(cfg);
    auto result = cmd.execute(make_config_args("get", {}, /*all_flag=*/true));
    EXPECT_EQ(result.exit_code, 0);
    // Output should mention at least one key
    EXPECT_NE(result.message.find("cpu.mode"), std::string::npos);
}

TEST(ConfigCommandTest, GetSingleKeyReturnsValue) {
    Config cfg;
    ConfigCommand cmd(cfg);
    auto result = cmd.execute(make_config_args("get", {"cpu.mode"}));
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.message.find("interpreter"), std::string::npos);
}

TEST(ConfigCommandTest, GetUnknownKeyReturnsFailure) {
    Config cfg;
    ConfigCommand cmd(cfg);
    auto result = cmd.execute(make_config_args("get", {"bad.key"}));
    EXPECT_NE(result.exit_code, 0);
}

TEST(ConfigCommandTest, SetValidKeyValueReturnsSuccess) {
    Config cfg;
    ConfigCommand cmd(cfg);
    auto result = cmd.execute(make_config_args("set", {"cpu.mode", "jit"}));
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(*cfg.get("cpu.mode"), "jit");
}

TEST(ConfigCommandTest, SetInvalidValueReturnsFailure) {
    Config cfg;
    ConfigCommand cmd(cfg);
    auto result = cmd.execute(make_config_args("set", {"cpu.mode", "bad"}));
    EXPECT_NE(result.exit_code, 0);
}

TEST(ConfigCommandTest, SetMissingArgsReturnsFailure) {
    Config cfg;
    ConfigCommand cmd(cfg);
    auto result = cmd.execute(make_config_args("set", {"cpu.mode"}));
    EXPECT_NE(result.exit_code, 0);
}

TEST(ConfigCommandTest, ResetSingleKeyRestoresDefault) {
    Config cfg;
    cfg.set("log.level", "debug");
    ConfigCommand cmd(cfg);
    auto result = cmd.execute(make_config_args("reset", {"log.level"}));
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(*cfg.get("log.level"), "warn");
}

TEST(ConfigCommandTest, ResetAllRestoresDefaults) {
    Config cfg;
    cfg.set("cpu.mode", "jit");
    cfg.set("log.level", "trace");
    ConfigCommand cmd(cfg);
    auto result = cmd.execute(make_config_args("reset"));
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(*cfg.get("cpu.mode"), "interpreter");
    EXPECT_EQ(*cfg.get("log.level"), "warn");
}

TEST(ConfigCommandTest, UnknownSubcommandReturnsFailure) {
    Config cfg;
    ConfigCommand cmd(cfg);
    auto result = cmd.execute(make_config_args("badcmd"));
    EXPECT_NE(result.exit_code, 0);
}

TEST(ConfigCommandTest, GetWithNoKeyAndNoAllFlagListsAll) {
    Config cfg;
    ConfigCommand cmd(cfg);
    auto result = cmd.execute(make_config_args("get"));
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.message.find("log.level"), std::string::npos);
}
