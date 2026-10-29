#pragma once
/// @file window_presentation.hpp
/// @brief Win32-signal classifier for Cocoa HWND hosting.
///
/// Guest-visible GetWindowRect / GetClientRect / WM_SIZE stay Win32. This
/// type only decides how AppKit hosts the HWND. Classify from WS_*/WS_EX_*
/// and the flags below — never from executable or vendor identity.
///
/// Policy: .docs/windowing-native-presentation.md

#include "windowing/model/window_types.hpp"

namespace vitreo::windowing {

namespace WS_EX {
    constexpr u32 DLGMODALFRAME = 0x00000001u;
    constexpr u32 TOPMOST       = 0x00000008u;
    constexpr u32 TRANSPARENT   = 0x00000020u;
    constexpr u32 MDICHILD      = 0x00000040u;
    constexpr u32 TOOLWINDOW    = 0x00000080u;
    constexpr u32 APPWINDOW     = 0x00040000u;
    constexpr u32 LAYERED       = 0x00080000u;
    constexpr u32 NOACTIVATE    = 0x08000000u;
} // namespace WS_EX

enum class WindowPresentationKind : u8 {
    MessageOnly,
    ChildView,
    OwnedTransient,
    ToolPalette,
    StockDialog,
    WizardPage,
    CompositionHost,
    SplashSkin,
    LayeredSurface,
    GuestOwnedFrame,
    StockDocument,
    ExclusiveFullscreen,
};

enum class PresentationChrome : u8 { None, Native, Guest };
enum class PresentationHost : u8 { None, View, Window, Panel };
enum class PresentationLevel : u8 { Normal, Elevated };

struct PresentationSignals {
    u32  style = 0;
    u32  ex_style = 0;
    bool is_dialog = false;
    bool is_message_only = false;
    bool client_drawn_frame = false;
    bool has_owner = false;
    bool owns_other_toplevels = false;
    bool exclusive_fullscreen = false;
};

struct PresentationRecipe {
    WindowPresentationKind kind = WindowPresentationKind::StockDocument;
    PresentationChrome chrome = PresentationChrome::Native;
    PresentationHost host = PresentationHost::Window;
    bool dock_tile = true;
    bool space_fullscreen = false;
    bool clear_host = false;
    bool activating = true;
    bool shadow = true;
    bool hide_system_ui = false;
    PresentationLevel window_level = PresentationLevel::Normal;
};

[[nodiscard]] bool presentation_has_caption(u32 style) noexcept;
[[nodiscard]] bool presentation_is_borderless_popup(u32 style) noexcept;
/// Learn taskbar / extended styles: eligibility of a visible top-level HWND.
/// APPWINDOW overrides owner, TOOLWINDOW and NOACTIVATE exclusion; it does
/// not make the window activating. Child/message-only HWNDs never qualify.
[[nodiscard]] bool presentation_has_taskbar_identity(
    const PresentationSignals& signals) noexcept;
[[nodiscard]] bool presentation_is_borderless_fullscreen(
    const PresentationSignals& signals, bool has_gpu_surface,
    const Rect& client, const Rect& monitor) noexcept;

[[nodiscard]] WindowPresentationKind classify_window_presentation(
    const PresentationSignals& signals) noexcept;

[[nodiscard]] PresentationRecipe make_presentation_recipe(
    const PresentationSignals& signals) noexcept;

[[nodiscard]] inline PresentationSignals make_presentation_signals(
    u32 style, u32 ex_style, bool is_dialog, bool client_drawn_frame,
    bool has_owner, bool owns_other_toplevels,
    bool exclusive_fullscreen) noexcept {
    PresentationSignals s{};
    s.style = style;
    s.ex_style = ex_style;
    s.is_dialog = is_dialog;
    s.client_drawn_frame = client_drawn_frame;
    s.has_owner = has_owner;
    s.owns_other_toplevels = owns_other_toplevels;
    s.exclusive_fullscreen = exclusive_fullscreen;
    return s;
}

} // namespace vitreo::windowing
