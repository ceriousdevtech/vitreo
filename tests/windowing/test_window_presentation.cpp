/// @file test_window_presentation.cpp
/// @brief Table-driven Win32 → Cocoa presentation classifier tests.

#include <gtest/gtest.h>
#include <windowing/cocoa/window_presentation.hpp>
#include <windowing/model/window_types.hpp>

using namespace vitreo::windowing;

namespace {

PresentationSignals sig() { return {}; }

PresentationRecipe recipe(PresentationSignals s) {
    return make_presentation_recipe(s);
}

} // namespace

TEST(WindowPresentationTest, MessageOnlyHasNoHost) {
    auto s = sig();
    s.is_message_only = true;
    s.style = WS::OVERLAPPEDWINDOW;
    const auto r = recipe(s);
    EXPECT_EQ(r.kind, WindowPresentationKind::MessageOnly);
    EXPECT_EQ(r.host, PresentationHost::None);
    EXPECT_EQ(r.chrome, PresentationChrome::None);
    EXPECT_FALSE(r.dock_tile);
    EXPECT_FALSE(r.space_fullscreen);
}

TEST(WindowPresentationTest, BorderlessGpuMonitorWindowUsesSpace) {
    auto s = sig();
    s.style = WS::POPUP | WS::VISIBLE;
    const Rect monitor{1920, 0, 4480, 1440};
    EXPECT_TRUE(presentation_is_borderless_fullscreen(s, true, monitor, monitor));
    EXPECT_FALSE(presentation_is_borderless_fullscreen(s, false, monitor, monitor));
    EXPECT_FALSE(presentation_is_borderless_fullscreen(
        s, true, Rect{1920, 0, 3200, 720}, monitor));
    s.has_owner = true;
    EXPECT_FALSE(presentation_is_borderless_fullscreen(s, true, monitor, monitor));
    s.has_owner = false;
    s.owns_other_toplevels = true;
    EXPECT_FALSE(presentation_is_borderless_fullscreen(s, true, monitor, monitor));
    s.owns_other_toplevels = false;
    s.ex_style = WS_EX::TOOLWINDOW;
    EXPECT_FALSE(presentation_is_borderless_fullscreen(s, true, monitor, monitor));
    s.ex_style = 0;
    s.style = WS::OVERLAPPEDWINDOW;
    EXPECT_FALSE(presentation_is_borderless_fullscreen(s, true, monitor, monitor));
    s.client_drawn_frame = true;
    EXPECT_TRUE(presentation_is_borderless_fullscreen(s, true, monitor, monitor));
}

TEST(WindowPresentationTest, ChildViewStaysAView) {
    auto s = sig();
    s.style = WS::CHILD | WS::VISIBLE;
    const auto r = recipe(s);
    EXPECT_EQ(r.kind, WindowPresentationKind::ChildView);
    EXPECT_EQ(r.host, PresentationHost::View);
    EXPECT_FALSE(r.dock_tile);
    EXPECT_FALSE(r.space_fullscreen);
}

TEST(WindowPresentationTest, MdiChildDoesNotPromote) {
    auto s = sig();
    s.style = WS::CHILD | WS::CAPTION;
    s.ex_style = WS_EX::MDICHILD;
    const auto r = recipe(s);
    EXPECT_EQ(r.kind, WindowPresentationKind::ChildView);
    EXPECT_EQ(r.host, PresentationHost::View);
}

TEST(WindowPresentationTest, OwnedTransientIsNonactivatingPanel) {
    auto s = sig();
    s.style = WS::POPUP;
    s.ex_style = WS_EX::NOACTIVATE | WS_EX::TOOLWINDOW;
    s.has_owner = true;
    const auto r = recipe(s);
    EXPECT_EQ(r.kind, WindowPresentationKind::OwnedTransient);
    EXPECT_EQ(r.host, PresentationHost::Panel);
    EXPECT_EQ(r.chrome, PresentationChrome::Guest);
    EXPECT_FALSE(r.dock_tile);
    EXPECT_FALSE(r.activating);
    EXPECT_FALSE(r.space_fullscreen);
}

TEST(WindowPresentationTest, ToolPaletteHasNoDockUnlessAppWindow) {
    auto s = sig();
    s.style = WS::CAPTION | WS::SYSMENU;
    s.ex_style = WS_EX::TOOLWINDOW;
    auto r = recipe(s);
    EXPECT_EQ(r.kind, WindowPresentationKind::ToolPalette);
    EXPECT_EQ(r.chrome, PresentationChrome::Native);
    EXPECT_FALSE(r.dock_tile);
    EXPECT_FALSE(r.space_fullscreen);

    s.ex_style |= WS_EX::APPWINDOW;
    r = recipe(s);
    EXPECT_EQ(r.kind, WindowPresentationKind::ToolPalette);
    EXPECT_TRUE(r.dock_tile);
}

TEST(WindowPresentationTest, TaskbarIdentityFollowsExtendedStyleAndOwnerRules) {
    // Source: Learn "The Taskbar" / "Extended Window Styles". APPWINDOW
    // forces a visible top-level; NOACTIVATE retains its activation contract.
    struct Row { vitreo::u32 flags; bool unowned; bool owned; };
    const Row rows[] = {
        {0, true, false},
        {WS_EX::TOOLWINDOW, false, false},
        {WS_EX::NOACTIVATE, false, false},
        {WS_EX::TOOLWINDOW | WS_EX::NOACTIVATE, false, false},
        {WS_EX::APPWINDOW, true, true},
        {WS_EX::APPWINDOW | WS_EX::TOOLWINDOW, true, true},
        {WS_EX::APPWINDOW | WS_EX::NOACTIVATE, true, true},
        {WS_EX::APPWINDOW | WS_EX::TOOLWINDOW | WS_EX::NOACTIVATE, true, true},
    };
    for (const auto& row : rows) {
        for (bool owned : {false, true}) {
            auto s = sig();
            s.style = WS::CAPTION | WS::POPUP;
            s.ex_style = row.flags;
            s.has_owner = owned;
            EXPECT_EQ(presentation_has_taskbar_identity(s), owned ? row.owned : row.unowned);
            if ((row.flags & WS_EX::APPWINDOW) != 0) EXPECT_TRUE(recipe(s).dock_tile);
            if ((row.flags & WS_EX::NOACTIVATE) != 0) EXPECT_FALSE(recipe(s).activating);
            s.style |= WS::CHILD;
            EXPECT_FALSE(presentation_has_taskbar_identity(s));
            s.style &= ~WS::CHILD;
            s.is_message_only = true;
            EXPECT_FALSE(presentation_has_taskbar_identity(s));
        }
    }
}

TEST(WindowPresentationTest, StockDialogKeepsNativeChrome) {
    auto s = sig();
    s.style = WS::CAPTION | WS::SYSMENU | WS::POPUP;
    s.is_dialog = true;
    s.has_owner = true;
    const auto r = recipe(s);
    EXPECT_EQ(r.kind, WindowPresentationKind::StockDialog);
    EXPECT_EQ(r.host, PresentationHost::Window);
    EXPECT_EQ(r.chrome, PresentationChrome::Native);
    EXPECT_FALSE(r.dock_tile);
    EXPECT_FALSE(r.space_fullscreen);
    EXPECT_FALSE(r.clear_host);
}

TEST(WindowPresentationTest, WizardPageEmbeds) {
    auto s = sig();
    s.style = WS::POPUP | WS::VISIBLE;
    s.is_dialog = true;
    s.has_owner = true;
    const auto r = recipe(s);
    EXPECT_EQ(r.kind, WindowPresentationKind::WizardPage);
    EXPECT_EQ(r.host, PresentationHost::View);
    EXPECT_FALSE(r.dock_tile);
}

TEST(WindowPresentationTest, CompositionHostDefersToOwned) {
    auto s = sig();
    s.style = WS::POPUP;
    s.owns_other_toplevels = true;
    const auto r = recipe(s);
    EXPECT_EQ(r.kind, WindowPresentationKind::CompositionHost);
    EXPECT_EQ(r.chrome, PresentationChrome::Guest);
    EXPECT_FALSE(r.dock_tile);
    EXPECT_FALSE(r.space_fullscreen);
}

TEST(WindowPresentationTest, SplashSkinIsBorderlessNoDock) {
    auto s = sig();
    s.style = WS::POPUP;
    const auto r = recipe(s);
    EXPECT_EQ(r.kind, WindowPresentationKind::SplashSkin);
    EXPECT_EQ(r.chrome, PresentationChrome::Guest);
    EXPECT_FALSE(r.dock_tile);
    EXPECT_FALSE(r.space_fullscreen);
}

TEST(WindowPresentationTest, LayeredSurfaceIsClearHost) {
    auto s = sig();
    s.style = WS::POPUP;
    s.ex_style = WS_EX::LAYERED;
    const auto r = recipe(s);
    EXPECT_EQ(r.kind, WindowPresentationKind::LayeredSurface);
    EXPECT_TRUE(r.clear_host);
    EXPECT_FALSE(r.shadow);
}

TEST(WindowPresentationTest, GuestOwnedFrameIsSpaceCapable) {
    auto s = sig();
    s.style = WS::POPUP;
    s.client_drawn_frame = true;
    const auto r = recipe(s);
    EXPECT_EQ(r.kind, WindowPresentationKind::GuestOwnedFrame);
    EXPECT_EQ(r.chrome, PresentationChrome::Guest);
    EXPECT_TRUE(r.space_fullscreen);
    EXPECT_TRUE(r.dock_tile);
}

TEST(WindowPresentationTest, StockDocumentIsNativeSpaceAndDock) {
    auto s = sig();
    s.style = WS::OVERLAPPEDWINDOW;
    const auto r = recipe(s);
    EXPECT_EQ(r.kind, WindowPresentationKind::StockDocument);
    EXPECT_EQ(r.chrome, PresentationChrome::Native);
    EXPECT_TRUE(r.dock_tile);
    EXPECT_TRUE(r.space_fullscreen);
    EXPECT_TRUE(r.activating);
    EXPECT_FALSE(r.hide_system_ui);
}

TEST(WindowPresentationTest, ExclusiveFullscreenUsesSpace) {
    auto s = sig();
    s.style = WS::POPUP;
    s.exclusive_fullscreen = true;
    const auto r = recipe(s);
    EXPECT_EQ(r.kind, WindowPresentationKind::ExclusiveFullscreen);
    EXPECT_TRUE(r.space_fullscreen);
    EXPECT_EQ(r.chrome, PresentationChrome::Guest);
    EXPECT_TRUE(r.hide_system_ui);
}

TEST(WindowPresentationTest, TopmostElevatesAnyKind) {
    auto s = sig();
    s.style = WS::OVERLAPPEDWINDOW;
    s.ex_style = WS_EX::TOPMOST;
    const auto r = recipe(s);
    EXPECT_EQ(r.kind, WindowPresentationKind::StockDocument);
    EXPECT_EQ(r.window_level, PresentationLevel::Elevated);
}

TEST(WindowPresentationTest, NoActivateDisablesKey) {
    auto s = sig();
    s.style = WS::OVERLAPPEDWINDOW;
    s.ex_style = WS_EX::NOACTIVATE;
    const auto r = recipe(s);
    EXPECT_FALSE(r.activating);
}
