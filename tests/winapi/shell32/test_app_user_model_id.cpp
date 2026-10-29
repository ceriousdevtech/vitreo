// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_app_user_model_id.cpp
/// @brief The identity the Dock groups by (Learn "Application User Model
///        IDs": window-level ID over process-level ID over the system's own).

#include "winapi/system/shell32/app_user_model_id.hpp"

#include "ipc/hwnd_registry.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

namespace vitreo::winapi::shell {
namespace {

TEST(AppUserModelId, WindowIdOverridesProcessIdOverridesImage) {
    ASSERT_EQ(::unsetenv("VITREO_IPC_SCOPE"), 0);
    constexpr u32 hwnd = 0x00077001u;
    const std::optional<std::u16string> none;
    const std::optional<std::u16string> steam = u"Valve.Steam.Client";
    // System-assigned: by executable, case-insensitive, either separator.
    EXPECT_EQ(effective_app_user_model_id(hwnd, none, "C:\\Games\\Game.exe"),
              "image:C:\\GAMES\\GAME.EXE");
    EXPECT_EQ(effective_app_user_model_id(0, none, "c:/games/game.EXE"),
              "image:C:\\GAMES\\GAME.EXE");
    // "A window-level AppUserModelID overrides a process-level one."
    EXPECT_EQ(effective_app_user_model_id(hwnd, steam, "C:\\Steam\\steam.exe"),
              "id:Valve.Steam.Client");
    ASSERT_TRUE(ipc::hwnd_registry_update_shell_properties(
        hwnd, [](std::vector<ipc::WindowShellProperty>& values) {
            values.push_back({kAppUserModelFmtid, kAppUserModelIdPid, u"  Valve.Steam.Game  "});
        }));
    // Read as GetValue reads it: trimmed (window_propstore_probe "after-padded").
    EXPECT_EQ(window_app_user_model_id(hwnd), std::optional<std::u16string>(u"Valve.Steam.Game"));
    EXPECT_EQ(effective_app_user_model_id(hwnd, steam, "C:\\Steam\\steam.exe"),
              "id:Valve.Steam.Game");
    // The process-level identity still applies to a window without one.
    EXPECT_EQ(effective_app_user_model_id(0, steam, "C:\\Steam\\steam.exe"),
              "id:Valve.Steam.Client");
    // A blank window value is no value ("after-blank ... vt=0").
    ASSERT_TRUE(ipc::hwnd_registry_update_shell_properties(
        hwnd, [](std::vector<ipc::WindowShellProperty>& values) { values[0].text = u"   "; }));
    EXPECT_FALSE(window_app_user_model_id(hwnd).has_value());
    EXPECT_EQ(effective_app_user_model_id(hwnd, steam, "C:\\Steam\\steam.exe"),
              "id:Valve.Steam.Client");
    ipc::hwnd_registry_unpublish(hwnd);
}

TEST(AppUserModelId, SchemaTypesFollowPropkey) {
    EXPECT_EQ(shell_property_type(kAppUserModelFmtid, 5), ShellPropertyType::String);
    EXPECT_EQ(shell_property_type(kAppUserModelFmtid, 2), ShellPropertyType::String);
    EXPECT_EQ(shell_property_type(kAppUserModelFmtid, 9), ShellPropertyType::Boolean);
    EXPECT_EQ(shell_property_type(kAppUserModelFmtid, 12), ShellPropertyType::Unconverted);
    EXPECT_EQ(shell_property_type(kAppUserModelFmtid, 26), ShellPropertyType::Unconverted);
    EXPECT_EQ(shell_property_type(kAppUserModelFmtid, 1000), ShellPropertyType::Unknown);
}

} // namespace
} // namespace vitreo::winapi::shell
