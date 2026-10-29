// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_dock_session_election.cpp
/// @brief One Dock tile per AppUserModelID group (Learn "Application User
///        Model IDs": the taskbar groups by ID, not by process tree).

#include "windowing/cocoa/dock_session_election.hpp"

#include <gtest/gtest.h>

namespace vitreo::windowing {
namespace {

const std::string kSteam = "id:Valve.Steam.Client";
const std::string kGame = "image:C:\\GAMES\\GAME.EXE";

TEST(DockSessionElection, ChildWithTheRootsIdJoinsTheRootsTile) {
    DockSessionMembers members;
    members[100] = {10, 0, 0, true, true, kSteam};       // launcher, tray only
    members[200] = {20, 5, 0x10, false, false, kSteam};  // helper with a window
    EXPECT_EQ(dock_session_group_owner(members, kSteam), 100);
    // Reopen goes to the group's window, wherever it lives.
    EXPECT_EQ(dock_session_group_target(members, kSteam), 200);
}

TEST(DockSessionElection, ChildWithAnotherIdOwnsItsOwnTileOnceItHasAWindow) {
    DockSessionMembers members;
    members[100] = {10, 0, 0, true, true, kSteam};
    members[300] = {30, 0, 0, false, false, kGame};
    // A windowless process of another ID (a service, a crash handler) shows
    // nothing: only the launch root owns a tile before it has a window.
    EXPECT_EQ(dock_session_group_owner(members, kGame), 0);
    EXPECT_EQ(dock_session_group_owner(members, kSteam), 100);
    members[300].hwnd = 0x20;
    EXPECT_EQ(dock_session_group_owner(members, kGame), 300);
    EXPECT_EQ(dock_session_group_owner(members, kSteam), 100);
    // Each group routes reopen only among its own members.
    EXPECT_EQ(dock_session_group_target(members, kGame), 300);
    EXPECT_EQ(dock_session_group_target(members, kSteam), 100);
}

TEST(DockSessionElection, OldestSurvivorOfAGroupTakesOverAndRootWins) {
    DockSessionMembers members;
    members[400] = {40, 0, 0x30, false, false, kGame};
    members[300] = {30, 0, 0x20, false, false, kGame};
    EXPECT_EQ(dock_session_group_owner(members, kGame), 300);
    members.erase(300);
    EXPECT_EQ(dock_session_group_owner(members, kGame), 400);
    members[500] = {50, 0, 0, true, false, kGame};
    EXPECT_EQ(dock_session_group_owner(members, kGame), 500);
}

TEST(DockSessionElection, MostRecentlyActiveWindowIsTheReopenTarget) {
    DockSessionMembers members;
    members[100] = {10, 7, 0x10, true, false, kSteam};
    members[200] = {20, 9, 0x20, false, false, kSteam};
    members[300] = {30, 99, 0x30, false, false, kGame};
    EXPECT_EQ(dock_session_group_target(members, kSteam), 200);
}

} // namespace
} // namespace vitreo::windowing
