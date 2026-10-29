/// @file window_presentation.cpp
/// @brief Win32-signal → Cocoa host recipe (WINDOW presentation catalog).

#include "windowing/cocoa/window_presentation.hpp"

namespace vitreo::windowing {

bool presentation_has_caption(u32 style) noexcept {
    return (style & WS::CAPTION) == WS::CAPTION;
}

bool presentation_is_borderless_popup(u32 style) noexcept {
    return (style & WS::POPUP) != 0 && (style & WS::CHILD) == 0 &&
           !presentation_has_caption(style);
}

bool presentation_has_taskbar_identity(const PresentationSignals& s) noexcept {
    if (s.is_message_only || (s.style & WS::CHILD) != 0) return false;
    if ((s.ex_style & WS_EX::APPWINDOW) != 0) return true;
    return !s.has_owner &&
        (s.ex_style & (WS_EX::TOOLWINDOW | WS_EX::NOACTIVATE)) == 0;
}

bool presentation_is_borderless_fullscreen(
    const PresentationSignals& s, bool has_gpu_surface,
    const Rect& client, const Rect& monitor) noexcept {
    return has_gpu_surface && !s.is_dialog && !s.is_message_only &&
           !s.has_owner && !s.owns_other_toplevels &&
           (s.style & WS::CHILD) == 0 &&
           (s.ex_style & (WS_EX::TOOLWINDOW | WS_EX::NOACTIVATE |
                          WS_EX::LAYERED)) == 0 &&
           (s.client_drawn_frame || presentation_is_borderless_popup(s.style)) &&
           monitor.width() > 0 && monitor.height() > 0 &&
           client.left == monitor.left && client.top == monitor.top &&
           client.right == monitor.right && client.bottom == monitor.bottom;
}

WindowPresentationKind classify_window_presentation(
    const PresentationSignals& s) noexcept {
    if (s.is_message_only) {
        return WindowPresentationKind::MessageOnly;
    }
    if ((s.style & WS::CHILD) != 0) {
        return WindowPresentationKind::ChildView;
    }
    if (s.exclusive_fullscreen) {
        return WindowPresentationKind::ExclusiveFullscreen;
    }

    const bool caption = presentation_has_caption(s.style);
    const bool tool = (s.ex_style & WS_EX::TOOLWINDOW) != 0;
    const bool noactivate = (s.ex_style & WS_EX::NOACTIVATE) != 0;
    const bool layered = (s.ex_style & WS_EX::LAYERED) != 0;

    if (s.has_owner && !caption && !s.is_dialog && (noactivate || tool)) {
        return WindowPresentationKind::OwnedTransient;
    }
    if (tool && caption) {
        return WindowPresentationKind::ToolPalette;
    }
    if (s.is_dialog && caption) {
        return WindowPresentationKind::StockDialog;
    }
    if (s.is_dialog && !caption && s.has_owner) {
        return WindowPresentationKind::WizardPage;
    }
    if (presentation_is_borderless_popup(s.style) && s.owns_other_toplevels) {
        return WindowPresentationKind::CompositionHost;
    }
    if (layered) {
        return WindowPresentationKind::LayeredSurface;
    }
    if (s.client_drawn_frame ||
        ((s.style & WS::THICKFRAME) != 0 && !caption && !s.is_dialog)) {
        return WindowPresentationKind::GuestOwnedFrame;
    }
    if (presentation_is_borderless_popup(s.style) || !caption) {
        return WindowPresentationKind::SplashSkin;
    }
    return WindowPresentationKind::StockDocument;
}

PresentationRecipe make_presentation_recipe(
    const PresentationSignals& s) noexcept {
    PresentationRecipe r{};
    r.kind = classify_window_presentation(s);
    r.window_level = (s.ex_style & WS_EX::TOPMOST) != 0
                         ? PresentationLevel::Elevated
                         : PresentationLevel::Normal;
    r.activating = (s.ex_style & WS_EX::NOACTIVATE) == 0;
    r.clear_host = (s.ex_style & WS_EX::LAYERED) != 0;
    r.shadow = !r.clear_host;

    switch (r.kind) {
    case WindowPresentationKind::MessageOnly:
        r.chrome = PresentationChrome::None;
        r.host = PresentationHost::None;
        r.dock_tile = false;
        r.space_fullscreen = false;
        r.activating = false;
        r.shadow = false;
        r.clear_host = false;
        break;
    case WindowPresentationKind::ChildView:
        r.chrome = PresentationChrome::None;
        r.host = PresentationHost::View;
        r.dock_tile = false;
        r.space_fullscreen = false;
        break;
    case WindowPresentationKind::OwnedTransient:
        r.chrome = PresentationChrome::Guest;
        r.host = PresentationHost::Panel;
        r.dock_tile = false;
        r.space_fullscreen = false;
        break;
    case WindowPresentationKind::ToolPalette:
        r.chrome = PresentationChrome::Native;
        r.host = PresentationHost::Window;
        r.dock_tile = (s.ex_style & WS_EX::APPWINDOW) != 0;
        r.space_fullscreen = false;
        break;
    case WindowPresentationKind::StockDialog:
        r.chrome = PresentationChrome::Native;
        r.host = PresentationHost::Window;
        r.dock_tile = false;
        r.space_fullscreen = false;
        r.clear_host = false;
        r.shadow = true;
        break;
    case WindowPresentationKind::WizardPage:
        r.chrome = PresentationChrome::None;
        r.host = PresentationHost::View;
        r.dock_tile = false;
        r.space_fullscreen = false;
        r.clear_host = false;
        r.shadow = false;
        break;
    case WindowPresentationKind::CompositionHost:
        r.chrome = PresentationChrome::Guest;
        r.host = PresentationHost::Window;
        r.dock_tile = false;
        r.space_fullscreen = false;
        break;
    case WindowPresentationKind::SplashSkin:
        r.chrome = PresentationChrome::Guest;
        r.host = PresentationHost::Window;
        r.dock_tile = false;
        r.space_fullscreen = false;
        break;
    case WindowPresentationKind::LayeredSurface:
        r.chrome = PresentationChrome::Guest;
        r.host = PresentationHost::Window;
        r.dock_tile = !s.has_owner;
        r.space_fullscreen = false;
        r.clear_host = true;
        r.shadow = false;
        break;
    case WindowPresentationKind::GuestOwnedFrame:
        r.chrome = PresentationChrome::Guest;
        r.host = PresentationHost::Window;
        r.dock_tile = true;
        r.space_fullscreen = true;
        break;
    case WindowPresentationKind::StockDocument:
        r.chrome = PresentationChrome::Native;
        r.host = PresentationHost::Window;
        r.dock_tile = true;
        r.space_fullscreen = true;
        break;
    case WindowPresentationKind::ExclusiveFullscreen:
        r.chrome = PresentationChrome::Guest;
        r.host = PresentationHost::Window;
        r.dock_tile = true;
        r.space_fullscreen = true;
        r.hide_system_ui = true;
        break;
    }

    // APPWINDOW explicitly forces a visible top-level onto the taskbar,
    // including owned dialogs/tool windows and nonactivating windows.
    if ((s.ex_style & WS_EX::APPWINDOW) != 0 &&
        (r.host == PresentationHost::Window || r.host == PresentationHost::Panel)) {
        r.dock_tile = presentation_has_taskbar_identity(s);
    }
    if ((s.ex_style & WS_EX::NOACTIVATE) != 0) {
        r.activating = false;
    }
    if (s.exclusive_fullscreen) {
        r.space_fullscreen = true;
    }
    return r;
}

} // namespace vitreo::windowing
