/// @file test_cocoa_bridge_owned_dialogs.cpp
/// @brief Tests for CocoaBridge owner and owned-dialog presentation and restack (WINDOW-003)

#include "cocoa_bridge_test_fixture.hpp"

TEST_F(CocoaBridgeTest, RestackDoesNotPresentOffMonitorOwnedWindow) {
    WindowData owner;
    owner.handle = 261;
    owner.rect = {80, 60, 80 + 400, 60 + 300};
    owner.client_rect = {0, 0, 400, 300};
    owner.visible = false;
    ASSERT_TRUE(bridge.create_window(261, owner).is_ok());
    bridge.set_visible(261, true);
    ASSERT_TRUE(bridge.native_window_is_visible(261));

    WindowData owned;
    owned.handle = 262;
    owned.owner = 261;
    owned.is_dialog = true;
    owned.rect = {20000, 100, 20400, 400};
    owned.client_rect = {0, 0, 400, 300};
    owned.visible = true;
    ASSERT_TRUE(bridge.create_window(262, owned).is_ok());
    bridge.set_visible(262, true);
    EXPECT_FALSE(bridge.native_window_is_visible(262));

    bridge.restack_owned_windows_above(261);
    EXPECT_FALSE(bridge.native_window_is_visible(262));
    EXPECT_TRUE(bridge.native_window_is_visible(261));
}

TEST_F(CocoaBridgeTest, CoveringOwnerWithOwnedDialogIsNotPresented) {
    // A display-covering WS_POPUP that owns dialogs is a composition host.
    // Presenting it on Next/Back is the black or chromeless duplicate plate.
    const auto mon = bridge.get_primary_monitor_info();
    WindowData host;
    host.handle = 280;
    host.style = WS::POPUP;
    host.rect = {mon.left, mon.top, mon.right, mon.bottom};
    host.client_rect = {0, 0, mon.width, mon.height};
    host.visible = true;
    ASSERT_TRUE(bridge.create_window(280, host).is_ok());
    ASSERT_TRUE(bridge.native_window_is_visible(280));

    WindowData page;
    page.handle = 281;
    page.owner = 280;
    page.is_dialog = true;
    page.style = WS::POPUP | WS::CAPTION | WS::SYSMENU;
    page.rect = {80, 60, 80 + 400, 60 + 300};
    page.client_rect = {0, 0, 400, 300};
    page.visible = true;
    ASSERT_TRUE(bridge.create_window(281, page).is_ok());
    EXPECT_TRUE(bridge.native_window_is_visible(281));
    EXPECT_FALSE(bridge.native_window_is_visible(280));

    bridge.set_visible(281, false);
    EXPECT_FALSE(bridge.native_window_is_visible(280));
}

TEST_F(CocoaBridgeTest, WizardSizedBorderlessOwnerWithOwnedDialogIsNotPresented) {
    WindowData host;
    host.handle = 286;
    host.style = WS::POPUP;
    host.rect = {80, 60, 80 + 500, 60 + 400};
    host.client_rect = {0, 0, 500, 400};
    host.visible = true;
    ASSERT_TRUE(bridge.create_window(286, host).is_ok());
    ASSERT_TRUE(bridge.native_window_is_visible(286));

    WindowData page;
    page.handle = 287;
    page.owner = 286;
    page.is_dialog = true;
    page.style = WS::POPUP | WS::CAPTION | WS::SYSMENU;
    page.rect = {80, 60, 80 + 500, 60 + 400};
    page.client_rect = {0, 0, 500, 400};
    page.visible = true;
    ASSERT_TRUE(bridge.create_window(287, page).is_ok());
    EXPECT_TRUE(bridge.native_window_is_visible(287));
    EXPECT_FALSE(bridge.native_window_is_visible(286));

    bridge.set_visible(287, false);
    EXPECT_FALSE(bridge.native_window_is_visible(286));
}

TEST_F(CocoaBridgeTest, CaptionedOwnerKeepsOwnedDialogNativeChrome) {
    // Stock DialogBox (Spider difficulty, etc.) is an owned top-level with
    // its own frame. Embedding it into the captioned owner was the
    // InstallShield extra-page path and produced a chromeless, click-through
    // plate over a disabled game window.
    WindowData game;
    game.handle = 293;
    game.style = WS::OVERLAPPEDWINDOW;
    game.rect = {80, 60, 80 + 800, 60 + 600};
    game.client_rect = {0, 0, 800, 600};
    game.visible = true;
    ASSERT_TRUE(bridge.create_window(293, game).is_ok());
    ASSERT_TRUE(bridge.has_native_window(293));

    WindowData difficulty;
    difficulty.handle = 294;
    difficulty.owner = 293;
    difficulty.is_dialog = true;
    difficulty.style = WS::POPUP | WS::CAPTION | WS::SYSMENU;
    difficulty.rect = {200, 180, 200 + 360, 180 + 220};
    difficulty.client_rect = {0, 0, 360, 220};
    difficulty.visible = true;
    ASSERT_TRUE(bridge.create_window(294, difficulty).is_ok());
    EXPECT_TRUE(bridge.has_native_window(294));
    EXPECT_FALSE(bridge.has_control_view(294));
    EXPECT_TRUE(bridge.native_window_is_opaque(294));
    EXPECT_FALSE(bridge.native_window_ignores_mouse_events(294));
    EXPECT_TRUE(bridge.native_content_hits_point(294, 180, 110));
}

TEST_F(CocoaBridgeTest, OwnedDialogEmbedsInFamilyPresentationWindow) {
    WindowData wizard;
    wizard.handle = 288;
    wizard.is_dialog = true;
    wizard.style = WS::POPUP | WS::CAPTION | WS::SYSMENU;
    wizard.rect = {80, 60, 80 + 500, 60 + 400};
    wizard.client_rect = {0, 0, 500, 400};
    wizard.visible = true;
    ASSERT_TRUE(bridge.create_window(288, wizard).is_ok());
    ASSERT_TRUE(bridge.has_native_window(288));

    WindowData page;
    page.handle = 289;
    page.owner = 288;
    page.is_dialog = true;
    page.style = WS::POPUP;
    page.rect = {0, 0, 500, 400};
    page.client_rect = {0, 0, 500, 400};
    page.visible = true;
    ASSERT_TRUE(bridge.create_window(289, page).is_ok());
    EXPECT_TRUE(bridge.has_control_view(289));
    EXPECT_FALSE(bridge.has_native_window(289));
    EXPECT_TRUE(bridge.native_window_is_visible(288));
}

// A modal DialogBox carries its own WS_CAPTION. Rule 1 / "Modal Win32
// dialogs" of .docs/windowing-native-presentation.md: it is a top-level window
// with native chrome, not a chromeless wizard page the family host paints in
// place. Embedding it drops the title bar and buries its controls in the
// NSWindow that DialogBox has just disabled for the owner.
TEST_F(CocoaBridgeTest, CaptionedOwnedDialogKeepsItsOwnNativeWindow) {
    WindowData main_dlg;
    main_dlg.handle = 293;
    main_dlg.is_dialog = true;
    main_dlg.style = WS::POPUP | WS::CAPTION | WS::SYSMENU;
    main_dlg.rect = {80, 60, 80 + 500, 60 + 400};
    main_dlg.client_rect = {0, 0, 500, 400};
    main_dlg.visible = true;
    ASSERT_TRUE(bridge.create_window(293, main_dlg).is_ok());
    ASSERT_TRUE(bridge.has_native_window(293));

    WindowData modal;
    modal.handle = 294;
    modal.owner = 293;
    modal.is_dialog = true;
    modal.style = WS::POPUP | WS::CAPTION | WS::SYSMENU;
    modal.rect = {120, 100, 120 + 320, 100 + 220};
    modal.client_rect = {0, 0, 320, 220};
    modal.visible = true;
    ASSERT_TRUE(bridge.create_window(294, modal).is_ok());

    EXPECT_TRUE(bridge.has_native_window(294));
    EXPECT_FALSE(bridge.has_control_view(294));
    EXPECT_TRUE(bridge.native_window_is_visible(294));
}

TEST_F(CocoaBridgeTest, ExistingOwnedDialogAdoptsWhenFamilyHostAppears) {
    WindowData skin;
    skin.handle = 290;
    skin.style = WS::POPUP;
    skin.rect = {80, 60, 80 + 500, 60 + 400};
    skin.client_rect = {0, 0, 500, 400};
    skin.visible = true;
    ASSERT_TRUE(bridge.create_window(290, skin).is_ok());

    WindowData early_page;
    early_page.handle = 291;
    early_page.owner = 290;
    early_page.is_dialog = true;
    early_page.style = WS::POPUP;
    early_page.rect = {80, 60, 80 + 500, 60 + 400};
    early_page.client_rect = {0, 0, 500, 400};
    early_page.visible = true;
    ASSERT_TRUE(bridge.create_window(291, early_page).is_ok());
    ASSERT_TRUE(bridge.has_native_window(291));

    WindowData wizard;
    wizard.handle = 292;
    wizard.owner = 290;
    wizard.is_dialog = true;
    wizard.style = WS::POPUP | WS::CAPTION | WS::SYSMENU;
    wizard.rect = {80, 60, 80 + 500, 60 + 400};
    wizard.client_rect = {0, 0, 500, 400};
    wizard.visible = true;
    ASSERT_TRUE(bridge.create_window(292, wizard).is_ok());

    EXPECT_TRUE(bridge.native_window_is_visible(292));
    EXPECT_TRUE(bridge.has_control_view(291));
    EXPECT_FALSE(bridge.native_window_is_visible(291));
    EXPECT_FALSE(bridge.native_window_is_visible(290));
}

TEST_F(CocoaBridgeTest, RestackDoesNotOrderInUnshownOwnedDialog) {
    WindowData owner;
    owner.handle = 282;
    owner.rect = {80, 60, 80 + 400, 60 + 300};
    owner.client_rect = {0, 0, 400, 300};
    owner.visible = false;
    ASSERT_TRUE(bridge.create_window(282, owner).is_ok());
    bridge.set_visible(282, true);
    ASSERT_TRUE(bridge.native_window_is_visible(282));

    WindowData shown;
    shown.handle = 283;
    shown.owner = 282;
    shown.is_dialog = true;
    shown.style = WS::POPUP | WS::CAPTION | WS::SYSMENU;
    shown.rect = {100, 80, 100 + 360, 80 + 260};
    shown.client_rect = {0, 0, 360, 260};
    shown.visible = true;
    ASSERT_TRUE(bridge.create_window(283, shown).is_ok());
    ASSERT_TRUE(bridge.native_window_is_visible(283));

    WindowData sibling;
    sibling.handle = 284;
    sibling.owner = 282;
    sibling.is_dialog = true;
    sibling.style = WS::POPUP;
    sibling.rect = {100, 80, 100 + 360, 80 + 260};
    sibling.client_rect = {0, 0, 360, 260};
    sibling.visible = false;
    ASSERT_TRUE(bridge.create_window(284, sibling).is_ok());
    EXPECT_FALSE(bridge.native_window_is_visible(284));

    bridge.restack_owned_windows_above(282);
    EXPECT_FALSE(bridge.native_window_is_visible(284));
    EXPECT_TRUE(bridge.native_window_is_visible(283));
}
