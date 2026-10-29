// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file dock_session_election.hpp
/// @brief Which process of a launch family shows a Dock tile, per
///        AppUserModelID group.
///
/// The Windows taskbar gives one button per AppUserModelID, not per process
/// tree (Learn "Application User Model IDs"): processes and windows that share
/// an ID share a button, and a process with a different ID -- a game a
/// launcher started -- gets its own. A family's members are therefore grouped
/// by the ID each publishes, and each group elects one Dock owner:
///   * the launch root may own its group before it shows a window (the
///     launched application's Dock tile appears at launch);
///   * any other member owns only while it has a taskbar-eligible window or
///     a notification-area reopen target;
///   * the root wins, then the oldest process.
/// Dock reopen in a group goes to its most recently active member.

#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include <sys/types.h>

namespace vitreo::windowing {

struct DockSessionMember {
    uint64_t birth{};
    uint64_t active{};
    uint32_t hwnd{};
    bool root{};
    bool tray{};
    std::string group;
};

using DockSessionMembers = std::map<pid_t, DockSessionMember>;

/// The member of @p group that shows the group's Dock tile, or 0.
[[nodiscard]] inline pid_t dock_session_group_owner(const DockSessionMembers& members,
                                                    const std::string& group) {
    pid_t result = 0;
    for (const auto& [pid, member] : members) {
        if (member.group != group) continue;
        if (!member.root && member.hwnd == 0 && !member.tray) continue;
        if (!result) {
            result = pid;
            continue;
        }
        const DockSessionMember& best = members.at(result);
        if ((member.root && !best.root) ||
            (member.root == best.root &&
             std::pair(member.birth, pid) < std::pair(best.birth, result))) {
            result = pid;
        }
    }
    return result;
}

/// The member of @p group a Dock reopen activates, or 0.
[[nodiscard]] inline pid_t dock_session_group_target(const DockSessionMembers& members,
                                                     const std::string& group) {
    pid_t result = 0;
    for (const auto& [pid, member] : members) {
        if (member.group != group) continue;
        if (member.hwnd == 0 && !member.tray) continue;
        if (!result || std::pair(member.active, member.birth) >
                           std::pair(members.at(result).active, members.at(result).birth)) {
            result = pid;
        }
    }
    return result;
}

} // namespace vitreo::windowing
