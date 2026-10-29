// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include <gtest/gtest.h>
#include <vitreo/common.hpp>

#include <cstdlib>
#include <optional>
#include <string>

using namespace vitreo;

namespace {

constexpr const char* kTestFlag = "VITREO_TEST_ENVIRONMENT_FLAG";

class EnvironmentFlagTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (const char* value = std::getenv(kTestFlag)) {
            m_original = value;
        }
        unsetenv(kTestFlag);
    }

    void TearDown() override {
        if (m_original) {
            setenv(kTestFlag, m_original->c_str(), 1);
        } else {
            unsetenv(kTestFlag);
        }
    }

private:
    std::optional<std::string> m_original;
};

TEST_F(EnvironmentFlagTest, MissingVariableUsesRequestedDefault) {
    EXPECT_TRUE(environment_flag_enabled(kTestFlag, true));
    EXPECT_FALSE(environment_flag_enabled(kTestFlag, false));
}

TEST_F(EnvironmentFlagTest, ExactZeroDisablesDefaultOnFeature) {
    ASSERT_EQ(setenv(kTestFlag, "0", 1), 0);
    EXPECT_FALSE(environment_flag_enabled(kTestFlag, true));
}

TEST_F(EnvironmentFlagTest, OneEnablesFeature) {
    ASSERT_EQ(setenv(kTestFlag, "1", 1), 0);
    EXPECT_TRUE(environment_flag_enabled(kTestFlag, false));
}

TEST_F(EnvironmentFlagTest, NonzeroTextRemainsBackwardCompatible) {
    ASSERT_EQ(setenv(kTestFlag, "enabled", 1), 0);
    EXPECT_TRUE(environment_flag_enabled(kTestFlag, false));
}

} // namespace
