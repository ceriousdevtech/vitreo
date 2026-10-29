#pragma once

/// @file notify_icons.hpp
/// @brief The notification area: Shell_NotifyIconA/W and Shell_NotifyIconGetRect.
///
/// Contract sources: Learn (Shell_NotifyIconW, NOTIFYICONDATAW,
/// Shell_NotifyIconGetRect) and three Windows probes run on a signed-in
/// Windows 11 desktop, x86 and x64 alike:
/// - notifyicon_layout_probe: the NOTIFYICONDATAA/W and NOTIFYICONIDENTIFIER
///   layouts the offsets below are copied from;
/// - notifyicon_tray_probe / s7_notifyicon_probe: results and last errors;
/// - notifyicon_click_probe: the callback messages a real click delivers.
///
/// Each icon is presented as a macOS menu-bar status item through the
/// Presenter hooks (the Cocoa bridge at run time, a fake in tests). A Dock
/// click on an app that shows no window activates its most recently added
/// visible icon with that icon's left-click sequence -- the macOS way back to
/// a window an app has hidden in the tray (.docs/windowing-native-presentation.md).

#include <vitreo/common.hpp>
#include <windowing/model/notify_icon_types.hpp>
#include <windowing/model/window_types.hpp>

#include <array>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::winapi {

class Marshaler;

/// Field offsets of one NOTIFYICONDATA flavour
/// (tools/oracle_probe/notifyicon_layout_probe.expected / .x86.expected).
struct NotifyIconDataLayout {
    u32 hwnd, uid, flags, callback_message, icon, tip, state, state_mask, info, version,
        info_title, info_flags, guid, balloon_icon;
    u32 v1_size, v2_size, v3_size, full_size;
    bool wide, is64;
};

inline constexpr NotifyIconDataLayout kNotifyIconDataW64{
    8, 16, 20, 24, 32, 40, 296, 300, 304, 816, 820, 948, 952, 968, 168, 952, 968, 976, true, true};
inline constexpr NotifyIconDataLayout kNotifyIconDataW32{
    4, 8, 12, 16, 20, 24, 280, 284, 288, 800, 804, 932, 936, 952, 152, 936, 952, 956, true, false};
inline constexpr NotifyIconDataLayout kNotifyIconDataA64{
    8, 16, 20, 24, 32, 40, 168, 172, 176, 432, 436, 500, 504, 520, 104, 504, 520, 528, false, true};
inline constexpr NotifyIconDataLayout kNotifyIconDataA32{
    4, 8, 12, 16, 20, 24, 152, 156, 160, 416, 420, 484, 488, 504, 88, 488, 504, 508, false, false};

class NotifyIconManager {
public:
    /// How the icons reach the screen. Unset hooks present nothing (tests,
    /// headless runs); GetRect then reports every icon as hidden.
    struct Presenter {
        std::function<void(u64 key, const windowing::NotifyIconPresentation&)> present;
        std::function<void(u64 key)> withdraw;
        /// The icon's rectangle in guest screen coordinates, or nullopt.
        std::function<std::optional<windowing::Rect>(u64 key)> frame;
        std::function<void(u64 key, const windowing::NotifyIconBalloon&)> show_balloon;
        std::function<void(u64 key)> hide_balloon;
    };
    /// The RGBA image (straight alpha, top-down) behind an HICON.
    struct IconImage {
        std::vector<u8> rgba;
        u32 width = 0;
        u32 height = 0;
    };

    void set_presenter(Presenter presenter);
    /// IsWindow: NIM_ADD needs a live owner window (probe "add id5 NULL hwnd",
    /// s7 "NIM_ADD bad hwnd").
    void set_window_check(std::function<bool(u32 hwnd)> check);
    /// PostMessage of a callback message to the owner window.
    void set_poster(std::function<void(u32 hwnd, u32 message, u64 wparam, u64 lparam)> poster);
    void set_icon_image(std::function<std::optional<IconImage>(u32 hicon)> image);

    /// Shell_NotifyIconA/W: TRUE or FALSE, with the last error set.
    u32 notify(Marshaler& m, u32 message, GuestAddr data, bool wide);
    /// Shell_NotifyIconGetRect: an HRESULT.
    u32 get_rect(Marshaler& m, GuestAddr identifier, GuestAddr rect);
    /// Host input on an icon: posts the callback messages Windows sends.
    void on_input(u64 key, windowing::NotifyIconInput input, i32 x, i32 y);
    /// The icon a Dock click activates: the most recently added visible one.
    [[nodiscard]] std::optional<u64> reopen_target() const;

private:
    struct Icon {
        u64 key = 0;
        u32 hwnd = 0;
        u32 uid = 0;
        bool has_guid = false;
        std::array<u8, 16> guid{};
        u32 callback_message = 0;  ///< 0 until NIF_MESSAGE: no callbacks
        u32 version = 0;           ///< NIM_SETVERSION: 0, 3 or 4
        u32 hicon = 0;
        std::string tip;
        u32 state = 0;             ///< NIS_* bits
        bool show_tip = false;     ///< NIF_SHOWTIP
    };
    struct PendingBalloon {
        u64 key;
        windowing::NotifyIconBalloon balloon;
    };
    struct Parsed;

    [[nodiscard]] Icon* find(const Parsed& p);
    [[nodiscard]] Icon* find_key(u64 key);
    void present(const Icon& icon);
    void post(const Icon& icon, u32 event, i32 x, i32 y);
    [[nodiscard]] std::pair<i32, i32> icon_anchor(u64 key) const;
    /// Take the next queued balloon if none is up; the returned action shows
    /// it and runs after m_mutex is released. Caller holds m_mutex.
    [[nodiscard]] std::function<void()> pump_balloons_locked();

    mutable std::recursive_mutex m_mutex;
    Presenter m_presenter;
    std::function<bool(u32)> m_window_check;
    std::function<void(u32, u32, u64, u64)> m_poster;
    std::function<std::optional<IconImage>(u32)> m_icon_image;
    std::vector<Icon> m_icons;  ///< in NIM_ADD order
    u64 m_next_key = 1;
    std::optional<u64> m_balloon_up;  ///< the icon whose balloon is showing
    std::deque<PendingBalloon> m_balloon_queue;
};

}  // namespace vitreo::winapi
