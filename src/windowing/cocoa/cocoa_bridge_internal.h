// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file cocoa_bridge_internal.h
/// @brief What cocoa_bridge.mm kept file-local, shared by its split parts.
///
/// Private to src/windowing/cocoa. Everything here was file-local to
/// cocoa_bridge.mm before it was split into the cocoa_*.mm files beside it,
/// and is now used by more than one of them. The definitions keep internal
/// linkage (static, or an anonymous namespace) and are only marked inline,
/// so each including translation unit gets its own copy, as the single
/// file had. A helper that must exist once is only declared here: the
/// cursor-rect pair shares one hidden-cursor singleton, the async hop shares
/// the main-thread work queue, and the overlay close and the presentation
/// recipe keep one definition so the compiler warnings their bodies carry
/// are reported once rather than once per including translation unit.

#import <Cocoa/Cocoa.h>

#include "windowing/cocoa/objc_helpers.h"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/cocoa/window_presentation.hpp"
#include "windowing/model/diag_flags.hpp"

namespace {
static inline bool diag_logs_enabled() noexcept {
    return vitreo::windowing::diag::diag_logs();
}



}

// Scale factor for converting Win32 pixel coordinates to Cocoa points.
// Guest D3D/Win32 sizes stay 1:1 with NSWindow points. Display-mode APIs
// that need the Retina framebuffer use get_screen_pixel_size() instead.
static constexpr CGFloat kWin32PixelScale = 1.0;

/// Height to flip Win32 top-down Y against.
///
/// Win32 screen coordinates are anchored to the PRIMARY monitor's top-left,
/// which AppKit exposes as `[[NSScreen screens] firstObject]` -- the screen
/// whose frame origin is (0,0). `[NSScreen mainScreen]` is the screen holding
/// the focused window and moves with the user, so flipping against it puts a
/// window on the wrong row the moment focus is on a display of a different
/// height or origin.
static inline CGFloat vtr_win32_flip_height(void) {
    NSArray<NSScreen*>* screens = [NSScreen screens];
    if ([screens count] == 0) return 1080.0;
    return NSMaxY([[screens objectAtIndex:0] frame]);
}

static constexpr vitreo::u32 kWsExTransparent = 0x00000020u;

static inline void apply_guest_click_through(NSWindow* window, vitreo::u32 ex_style) {
    if (window == nil) return;
    [window setIgnoresMouseEvents:(ex_style & kWsExTransparent) != 0];
}

/// A WS_EX_TRANSPARENT top-level window is an overlay: it is click-through by
/// contract and every pixel it shows is one the guest drew. AppKit's default
/// window plate is windowBackgroundColor, so when the guest composites such an
/// overlay through a path Vitreo does not carry — the guest's
/// UI compositor draws its tooltip that way and never presents a surface for
/// that HWND — the plate is all that ever appears, as an opaque light
/// rectangle sitting over the app. Nothing drawn must mean nothing shown.
///
/// Safe here precisely because the window is already click-through: the comment
/// on apply_composition_host_appearance warns that a non-opaque window is
/// hit-tested from pixel alpha, which would let clicks reach the HWND behind
/// it. That is the defined behaviour for WS_EX_TRANSPARENT, and
/// setIgnoresMouseEvents above has already asked for it.
[[nodiscard]] static inline bool guest_window_is_click_through_overlay(
    vitreo::u32 style, vitreo::u32 ex_style) {
    constexpr vitreo::u32 kWsPopup = 0x80000000u;
    return (ex_style & kWsExTransparent) != 0 && (style & kWsPopup) != 0;
}

static inline void apply_overlay_appearance(NSWindow* window) {
    if (window == nil) return;
    [window setOpaque:NO];
    [window setBackgroundColor:[NSColor clearColor]];
    [window setHasShadow:NO];
    NSView* content = [window contentView];
    if (content != nil && content.wantsLayer) {
        content.layer.backgroundColor = NSColor.clearColor.CGColor;
    }
}

/// UpdateLayeredWindow: DWM composes the premultiplied surface itself, so
/// there is no plate behind it and no system shadow, and a non-opaque window
/// is hit-tested from that same alpha, which is the layered-window rule.
static inline void apply_layered_alpha_appearance(NSWindow* window) {
    if (window == nil) return;
    [window setOpaque:NO];
    [window setBackgroundColor:[NSColor clearColor]];
    [window setHasShadow:NO];
    NSView* content = [window contentView];
    if (content != nil && content.wantsLayer) {
        content.layer.backgroundColor = NSColor.clearColor.CGColor;
    }
}

// Client-drawn / DComp hosts must not keep AppKit's opaque *white* plate —
// premultiplied swap-chain alpha composites onto that plate as a solid white
// window. They must still be AppKit-opaque: a non-opaque window is hit-tested
// from pixel alpha, and layer-hosting GDI/Metal plates do not feed that mask,
// so clicks activate the HWND behind the guest. Black is not the white plate.
/// The colour a guest window shows before it paints: its class brush's erase
/// colour, or opaque black for a NULL brush (Windows' unpainted redirection
/// surface; tools/oracle_probe/unpainted_window_probe). Dialogs keep AppKit's
/// face colour: they are native controls on DefDlgProc's COLOR_BTNFACE.
static inline NSColor* unpainted_plate_color(bool is_dialog,
                                      const std::optional<vitreo::u32>& erase_color) {
    if (is_dialog) return [NSColor windowBackgroundColor];
    if (!erase_color.has_value()) return [NSColor blackColor];
    const vitreo::u32 c = *erase_color;
    return [NSColor colorWithSRGBRed:static_cast<CGFloat>(c & 0xFF) / 255.0
                               green:static_cast<CGFloat>((c >> 8) & 0xFF) / 255.0
                                blue:static_cast<CGFloat>((c >> 16) & 0xFF) / 255.0
                               alpha:1.0];
}

static inline void apply_composition_host_appearance(NSWindow* window, bool clear_host,
                                              NSColor* plate = nil) {
    if (window == nil) return;
    if (clear_host) {
        [window setOpaque:YES];
        [window setBackgroundColor:[NSColor blackColor]];
        [window setHasShadow:NO];
        NSView* content = [window contentView];
        if (content != nil && content.wantsLayer) {
            content.layer.backgroundColor = NSColor.blackColor.CGColor;
        }
        return;
    }
    NSColor* color = plate != nil ? plate : [NSColor windowBackgroundColor];
    [window setOpaque:YES];
    [window setBackgroundColor:color];
    [window setHasShadow:YES];
    NSView* content = [window contentView];
    if (content != nil && content.wantsLayer) {
        content.layer.backgroundColor = color.CGColor;
    }
}

static inline void vtr_invalidate_cursor_rects_recursive(NSWindow* win, NSView* view) {
    if (win == nil || view == nil) return;
    [win invalidateCursorRectsForView:view];
    for (NSView* sub in view.subviews) {
        vtr_invalidate_cursor_rects_recursive(win, sub);
    }
}

@interface VtrContentView : NSView
- (NSTextField*)vtr_findEditableTextField;
- (id)vtr_textEditingTarget;
@end

@interface VtrWindow : NSWindow
@property (nonatomic, assign) vitreo::windowing::CocoaBridge* bridge;
/// Guest rect is off every monitor. Do not let AppKit clamp the frame onto
/// the display — Win32 would not present that HWND.
@property (nonatomic, assign) BOOL allowOffscreenFrame;
/// WS_EX_NOACTIVATE and owned transients must not take key.
@property (nonatomic, assign) BOOL allowKey;
@end

@interface VtrPanel : NSPanel
@property (nonatomic, assign) vitreo::windowing::CocoaBridge* bridge;
@property (nonatomic, assign) BOOL allowOffscreenFrame;
@property (nonatomic, assign) BOOL allowKey;
@end

// Defined in cocoa_cursor.mm. YES: applied a hidden or installed cursor.
// NO: caller may invoke super.
BOOL vtr_view_reset_cursor_rects(NSView* view);
BOOL vtr_view_cursor_update(NSView* view);

// Defined in cocoa_video_overlay.mm. Detaches the overlay window @p ptr
// from its parent window and closes it.
void vtr_close_video_overlay_window(void* ptr);

namespace vitreo::windowing {

// Defined in cocoa_main_thread.mm. Runs @p block on the main thread without
// waiting, through the same queue as vtr_run_on_main_sync.
void vtr_run_on_main_async(dispatch_block_t block);

// Defined in cocoa_window_style.mm.
void apply_presentation_recipe(NSWindow* win, const PresentationRecipe& recipe);

namespace {
inline void apply_space_fullscreen_collection_behavior(NSWindow* win, bool exclusive,
                                                u32 ws_style, u32 ex_style,
                                                bool is_dialog,
                                                bool client_drawn_frame);
inline bool window_is_space_fullscreen(NSWindow* win);
}
namespace {
inline PresentationSignals presentation_signals(u32 ws_style, u32 ex_style,
                                         bool is_dialog,
                                         bool client_drawn_frame,
                                         bool has_owner, bool owns_others,
                                         bool exclusive) {
    PresentationSignals s{};
    s.style = ws_style;
    s.ex_style = ex_style;
    s.is_dialog = is_dialog;
    s.client_drawn_frame = client_drawn_frame;
    s.has_owner = has_owner;
    s.owns_other_toplevels = owns_others;
    s.exclusive_fullscreen = exclusive;
    return s;
}

inline void apply_space_fullscreen_collection_behavior(NSWindow* win, bool exclusive,
                                                u32 ws_style, u32 ex_style,
                                                bool is_dialog,
                                                bool client_drawn_frame) {
    if (!win) return;
    apply_presentation_recipe(
        win, make_presentation_recipe(presentation_signals(
                 ws_style, ex_style, is_dialog, client_drawn_frame, false,
                 false, exclusive)));
}

inline bool window_is_space_fullscreen(NSWindow* win) {
    return win && (([win styleMask] & NSWindowStyleMaskFullScreen) != 0);
}
}  // namespace

} // namespace vitreo::windowing
