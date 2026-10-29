// SPDX-License-Identifier: LGPL-2.1-only

#include "platform/macos/elevation_authorization.hpp"

#include <gtest/gtest.h>

#include <string>

namespace vitreo::platform {
namespace {

TEST(ElevationAuthorizationTest, RejectsForgedCredentialWithoutInteraction) {
    auto authorizer = create_elevation_authorizer();
    ASSERT_NE(authorizer, nullptr);

    const std::string forged(32, '\0');
    EXPECT_FALSE(authorizer->validate_credential(forged));
    EXPECT_FALSE(authorizer->validate_credential("not-an-external-form"));
}

// Capsule elevation is granted without a host prompt: authorize() returns at
// once with a credential that validates, so a guest thread asking for
// elevation (Steam's install script on every Play) is never parked on a UI.
TEST(ElevationAuthorizationTest, AuthorizesWithoutInteractionAndCredentialValidates) {
    auto authorizer = create_elevation_authorizer();
    ASSERT_NE(authorizer, nullptr);

    ElevationRequest request{};
    request.application_name = "SteamService.exe";
    std::string credential;
    EXPECT_EQ(authorizer->authorize(request, credential),
              ElevationAuthorizationResult::Authorized);
    EXPECT_FALSE(credential.empty());
    EXPECT_TRUE(authorizer->validate_credential(credential));

    // Each grant mints its own credential.
    std::string second;
    ASSERT_EQ(authorizer->authorize(request, second),
              ElevationAuthorizationResult::Authorized);
    EXPECT_NE(second, credential);
    EXPECT_TRUE(authorizer->validate_credential(second));
}

} // namespace
} // namespace vitreo::platform
