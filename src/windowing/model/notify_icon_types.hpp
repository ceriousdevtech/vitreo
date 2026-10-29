#pragma once

/// @file notify_icon_types.hpp
/// @brief What the notification-area presenter shows and reports back.
///
/// shell32's Shell_NotifyIcon keeps the Win32 contract (identity, versions,
/// callback messages); the Cocoa bridge presents each icon as a menu-bar
/// status item (.docs/windowing-native-presentation.md §4) and reports input
/// on it. These are the types that cross that boundary.

#include <vitreo/common.hpp>

#include <string>
#include <vector>

namespace vitreo::windowing {

/// One notification-area icon as the host shows it.
struct NotifyIconPresentation {
    std::vector<u8> rgba;  ///< straight-alpha RGBA, top-down; empty: no image
    u32 width = 0;
    u32 height = 0;
    std::string tooltip;   ///< UTF-8 szTip
    /// Show tooltip natively. A NOTIFYICON_VERSION_4 icon without NIF_SHOWTIP
    /// gets NIN_POPUPOPEN/NIN_POPUPCLOSE instead (Learn, NIF_SHOWTIP).
    bool native_tooltip = true;
    bool hidden = false;   ///< NIS_HIDDEN
};

/// A balloon notification (NIF_INFO) shown from an icon.
struct NotifyIconBalloon {
    enum class Kind : u8 { None, Info, Warning, Error, User };
    std::string title;     ///< UTF-8 szInfoTitle
    std::string text;      ///< UTF-8 szInfo, never empty
    Kind kind = Kind::None;
    std::vector<u8> user_rgba;  ///< NIIF_USER image, straight-alpha RGBA
    u32 user_width = 0;
    u32 user_height = 0;
    bool large_icon = false;    ///< NIIF_LARGE_ICON
    bool sound = true;          ///< false for NIIF_NOSOUND
};

/// Input the host reports on an icon or its balloon.
enum class NotifyIconInput : u8 {
    HoverEnter,
    HoverExit,
    LeftDown,
    LeftDoubleDown,  ///< the second press of a double click
    LeftUp,
    RightDown,
    RightUp,
    BalloonShown,
    BalloonClicked,
    BalloonTimedOut,
    /// A Dock click on an app showing no window: the icon's primary click.
    Activate,
};

}  // namespace vitreo::windowing
