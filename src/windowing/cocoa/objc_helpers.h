// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file objc_helpers.h
/// @brief Internal surface shared between the cocoa_*.mm translation units.
///
/// Private to src/windowing/cocoa/ and cocoa_bridge.mm. Nothing outside the
/// Cocoa adapter may include it: it exposes Objective-C types, and keeping
/// those out of portable headers is the property that lets the rest of the
/// windowing layer stay host-neutral.
///
/// Everything here was file-local to cocoa_bridge.mm before that file was
/// split. This header exists to share it, not to publish it.

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>

#include "windowing/cocoa/cocoa_bridge.hpp"

#include <vitreo/common.hpp>

#include <atomic>
#include <cmath>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Views
// -----------------------------------------------------------------------------

/// Layer-backed view that blits a guest framebuffer via CGImage.
///
/// Unflipped: CALayer.contents keeps SoftBitmap row 0 at the visual top.
/// Guest coordinates stay Win32 Y-down — convert with
/// vtr_win32_point_in_view at this boundary.
@interface VtrPixelView : NSView
@property(nonatomic, assign) BOOL scalesFramebuffer;
@property(nonatomic, assign) BOOL passesThroughEvents;
@property(nonatomic, assign) int32_t framebufferWidth;
@property(nonatomic, assign) int32_t framebufferHeight;
/// Composite the framebuffer's fourth byte as premultiplied alpha. Set for a
/// WS_CHILD window's surface: a child shares its parent's pixels on Windows,
/// so what it never painted (A=0, SoftBitmap::child_window) shows through.
/// A top-level surface stays opaque.
@property(nonatomic, assign) BOOL compositesAlpha;
- (void)updatePixels:(const uint8_t*)rgba width:(int32_t)w height:(int32_t)h;
@end

/// Map a point in @p view to Win32 client space (origin top-left, +Y down).
static inline NSPoint vtr_win32_point_in_view(NSView* view, NSPoint p) {
    if (view != nil && ![view isFlipped]) {
        p.y = NSHeight(view.bounds) - p.y;
    }
    return p;
}

/// Metal-backed presentation view. Input distinguishes it from a pixel view
/// because the two map coordinates differently.
@interface VtrMetalView : NSView
@end

/// Plain container view for a Win32 control that has child views.
@interface VtrContainerView : NSView
@end

/// SysLink (WC_LINK) renderer. Flipped (Win32 Y-down). Draws the display text
/// with link styling and answers hit tests / ideal size from the same layout.
/// Mouse and keys are not consumed here: input reaches the HWND as WM_* and
/// user32's SysLink class procedure decides.
@interface VtrSysLinkView : NSView
- (void)setDisplay:(NSString*)display
            starts:(const uint32_t*)starts
           lengths:(const uint32_t*)lengths
            states:(const uint32_t*)states
             count:(size_t)count
          mnemonic:(int32_t)mnemonic
             style:(uint32_t)style;
- (void)setLinkFont:(NSFont*)font;
/// Item index under the view-local point, or -1.
- (int32_t)itemAtPoint:(NSPoint)point;
/// Laid-out size when wrapped at @p width (<= 0: unwrapped).
- (NSSize)idealSizeForWidth:(CGFloat)width;
@end

namespace vitreo::windowing {
/// The NSFont GDI uses for @p face_name (host substitution + line-box fit),
/// so native controls draw with the metrics the guest measured.
NSFont* vtr_gdi_matching_font(const std::u16string& face_name, float pt_size,
                              vitreo::u16 weight, bool italic);
}  // namespace vitreo::windowing

/// Load parsed SysLink markup and per-item LIS_* states into @p view. Main
/// thread only.
void vtr_syslink_apply(VtrSysLinkView* view, const vitreo::windowing::syslink::Markup& markup,
                       const std::vector<vitreo::u32>& states, vitreo::u32 style);

/// Add View → Enter Full Screen (Ctrl+Cmd+F) if the menu bar lacks it.
void vtr_ensure_enter_full_screen_menu(NSMenu* mainMenu);

/// Window delegate: close button to WM_CLOSE, live resize to WM_SIZE.
@interface VtrWindowDelegate : NSObject <NSWindowDelegate> {
    NSTimer* _liveResizeTimer;
    int32_t  _lastPostedW;   // last client size posted as WM_SIZE (-1 = none yet)
    int32_t  _lastPostedH;
}
@property (nonatomic, assign) vitreo::windowing::HWND hwnd;
@property (nonatomic, assign) vitreo::windowing::MessageQueue* queue;
@property (nonatomic, assign) vitreo::windowing::CocoaBridge* bridge;
@end

// -----------------------------------------------------------------------------
// Control-backing objects
// -----------------------------------------------------------------------------
//
// Declared here because both cocoa_bridge.mm (which creates the views) and
// cocoa_controls.mm (which implements the Win32 control messages against them)
// need them. Each carries the parent HWND so a delegate callback can be turned
// back into the WM_COMMAND or WM_NOTIFY Win32 expects.

/// One node in a SysTreeView32, addressed by its Win32 HTREEITEM.
@interface VtrTreeNode : NSObject
@property (nonatomic, assign) uint64_t hItem;
@property (nonatomic, strong) NSString* text;
@property (nonatomic, assign) BOOL hasChildren;
@property (nonatomic, strong) NSMutableArray<VtrTreeNode*>* children;
@end

/// Posts WM_COMMAND when an AppKit button is clicked.
@interface VtrButtonHandler : NSObject
@property (nonatomic, assign) vitreo::windowing::HWND parentHwnd;
@property (nonatomic, assign) vitreo::windowing::HWND controlHwnd;
@property (nonatomic, assign) uint32_t controlId;
@property (nonatomic, assign) vitreo::windowing::MessageQueue* queue;
@property (nonatomic, assign) vitreo::windowing::CocoaBridge* bridge;
- (void)buttonClicked:(id)sender;
@end

/// Posts WM_COMMAND when a native menu item is chosen.
@interface VtrMenuItemHandler : NSObject
@property (nonatomic, assign) vitreo::windowing::HWND targetHwnd;
@property (nonatomic, assign) uint16_t menuItemId;
@property (nonatomic, assign) vitreo::windowing::MessageQueue* queue;
- (void)menuItemClicked:(id)sender;
@end

/// SysTabControl32 presentation: left-aligned tab strip matching Win32
/// DocTab geometry. NSTabView's centered macOS tabs covered sibling editors
/// (Scintilla) and looked wrong for guest apps that host content as siblings.
@interface VtrTabStripView : NSView
@property(nonatomic, assign) vitreo::windowing::CocoaBridge* bridge;
@property(nonatomic, assign) vitreo::windowing::HWND hwnd;
@property(nonatomic, assign) BOOL suppressNotify;
- (void)reloadTabs;
@end

/// Retained for dialogs that still attach pages via add_container_view.
@interface VtrTabController : NSObject
@property(nonatomic, assign) vitreo::windowing::CocoaBridge* bridge;
@property(nonatomic, assign) vitreo::windowing::HWND hwnd;
@property(nonatomic, assign) BOOL suppressNotify;
@end

/// NSOutlineView data source and delegate backing SysTreeView32.
@interface VtrTreeController : NSObject <NSOutlineViewDataSource, NSOutlineViewDelegate>
@property (nonatomic, strong) NSMutableArray<VtrTreeNode*>* roots;
@property (nonatomic, strong) NSMutableDictionary<NSNumber*, VtrTreeNode*>* nodeByItem;
@property (nonatomic, assign) vitreo::windowing::CocoaBridge* bridge;
@property (nonatomic, assign) vitreo::windowing::HWND hwnd;
@property (nonatomic, assign) NSOutlineView* outline;
@property (nonatomic, assign) BOOL suppressNotify;
@end

// -----------------------------------------------------------------------------
// Win32 mouse-message wParam key-state flags (MK_*)
// -----------------------------------------------------------------------------

constexpr vitreo::u64 MK_LBUTTON = 0x0001;
constexpr vitreo::u64 MK_RBUTTON = 0x0002;
constexpr vitreo::u64 MK_SHIFT   = 0x0004;
constexpr vitreo::u64 MK_CONTROL = 0x0008;
constexpr vitreo::u64 MK_MBUTTON = 0x0010;

/// Every live menu-item handler.
///
/// Shared because the two halves of menu handling sit in different files: the
/// menu bar builds handlers, and the event pump refreshes their queue pointer
/// each time it runs, so a click posts to the queue that is current rather
/// than the one that existed when the menu was built.
[[nodiscard]] NSMutableArray* vtr_menu_handlers();
void vtr_set_menu_handlers(NSMutableArray* handlers);

/// True when the app bundle supplies its own icon, meaning the guest's icon
/// must not replace it.
[[nodiscard]] bool main_bundle_declares_application_icon();

/// Set the name macOS shows for this process — menu bar, Dock, Force Quit.
void set_process_application_name(NSString* name);

/// MK_* flags for the modifier keys and mouse buttons held during @p event.
[[nodiscard]] vitreo::u64 mouse_modifier_flags(NSEvent* event);

/// True when a native text editor has focus in @p win, meaning AppKit should
/// keep the keystroke rather than the guest receiving it.
[[nodiscard]] bool vtr_native_text_editor_focused(NSWindow* win);

// -----------------------------------------------------------------------------
// Shared helpers
// -----------------------------------------------------------------------------
//
// These sit at global scope rather than in vitreo::windowing because that is
// where cocoa_bridge.mm, cocoa_key_mapping.mm and cocoa_pointer_mapping.mm
// define them, outside the namespace. Moving them is a separate change;
// matching them is what this header is for.

/// Milliseconds from a monotonic clock that keeps counting across sleep —
/// the behavior GetTickCount promises.
[[nodiscard]] vitreo::u64 continuous_tick_count_ms() noexcept;

/// Pack the LPARAM Windows sends with WM_KEYDOWN/WM_KEYUP: repeat count, scan
/// code, extended-key flag, context code, previous state, and transition.
[[nodiscard]] vitreo::u64 key_message_lparam(vitreo::u32 vk,
                                             NSEventModifierFlags flags,
                                             bool key_down, bool was_down);

/// Map a point in an NSWindow's coordinate space to the top-level view.
/// Child targeting and drawable mapping happen later, exactly once, in
/// CocoaBridge::retarget_presentation_mouse. `clamp_outside` is accepted for
/// call-site compatibility; this function no longer maps the CAMetalLayer.
[[nodiscard]] bool map_native_mouse_point(NSWindow* window, NSPoint window_point,
                                          vitreo::i32& guest_x,
                                          vitreo::i32& guest_y,
                                          bool clamp_outside = false);

/// Map a window point through the presented guest surface (CAMetalLayer
/// drawable or scaled GDI framebuffer). Used by GetCursorPos so polling
/// matches WM_MOUSEMOVE after retarget.
[[nodiscard]] bool map_native_point_to_guest_surface(
    NSWindow* window, NSPoint window_point, bool clamp_outside,
    vitreo::i32& guest_x, vitreo::i32& guest_y);

/// Inverse of the hit-surface mapper; does not move the host cursor.
/// Points no presented surface owns map 1:1 through the content view, unless
/// @p require_surface: then they fail. A display-covering exclusive window
/// needs that -- its guest coordinates are the logical mode (640x480 etc.),
/// never desktop points -- so its callers can fall back to the whole display
/// while the swap chain has not presented yet.
[[nodiscard]] bool map_guest_surface_point_to_native(
    NSWindow* window, vitreo::i32 guest_x, vitreo::i32 guest_y, NSPoint& window_point,
    bool require_surface = false);

/// Cocoa points per backing pixel on @p screen (1 / backingScaleFactor).
[[nodiscard]] static inline CGFloat vtr_win32_points_per_pixel(NSScreen* screen) {
    if (screen == nil) {
        screen = [NSScreen mainScreen];
    }
    const CGFloat backing = (screen != nil) ? [screen backingScaleFactor] : 1.0;
    return (backing > 0.0) ? (1.0 / backing) : 1.0;
}

/// Host framebuffer size in device pixels. Prefers the current CoreGraphics
/// mode so a scaled "looks like" desktop still reports the real panel.
static inline void vtr_nsscreen_backing_pixels(NSScreen* screen,
                                               vitreo::i32& out_w,
                                               vitreo::i32& out_h) {
    out_w = 1920;
    out_h = 1080;
    if (screen == nil) {
        return;
    }
    NSNumber* screen_number =
        [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
    if (screen_number != nil) {
        const CGDirectDisplayID display_id =
            static_cast<CGDirectDisplayID>([screen_number unsignedIntValue]);
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display_id);
        if (mode != nullptr) {
            const size_t pixel_w = CGDisplayModeGetPixelWidth(mode);
            const size_t pixel_h = CGDisplayModeGetPixelHeight(mode);
            CGDisplayModeRelease(mode);
            if (pixel_w > 0 && pixel_h > 0) {
                out_w = static_cast<vitreo::i32>(pixel_w);
                out_h = static_cast<vitreo::i32>(pixel_h);
                return;
            }
        }
    }
    const CGFloat scale = vtr_win32_points_per_pixel(screen);
    const NSRect frame = [screen frame];
    out_w = static_cast<vitreo::i32>(std::lround(frame.size.width / scale));
    out_h = static_cast<vitreo::i32>(std::lround(frame.size.height / scale));
}

namespace vitreo::windowing {

/// Run a block on the main thread and wait for it. Safe to call from the main
/// thread, where it runs the block inline rather than deadlocking on itself.
void vtr_run_on_main_sync(dispatch_block_t block);

/// Run, on the main thread, every block other threads have queued for it
/// (vtr_run_on_main_sync / _bailout / async). A no-op elsewhere. A main thread
/// that waits for a guest thread calls this so that thread's own main-thread
/// requests cannot deadlock against the wait.
void vtr_drain_main_work();

/// As above, but gives up waiting when @p bailout_flag is set — for paths that
/// must not block indefinitely on a main thread that may itself be busy.
void vtr_run_on_main_sync_bailout(dispatch_block_t block,
                                  const std::atomic<bool>& bailout_flag,
                                  const std::atomic<bool>* bailout_flag2 = nullptr);

/// Convert a guest wide string to NSString. Guest text is UTF-16; NSString
/// takes the same units, so this is a length-counted copy rather than a
/// transcode.
[[nodiscard]] NSString* to_nsstring(const std::wstring& ws);

/// Convert NSString to guest wide text for GetWindowText / WM_GETTEXT.
/// Must not include a UTF-32 BOM or trailing NUL — `dataUsingEncoding:`
/// UTF-32LE prepends U+FEFF, which inflates the TCHAR count by one.
[[nodiscard]] std::wstring from_nsstring(NSString* s);

/// Remove Win32 accelerator markers from a label and report which character
/// was marked.
///
/// Win32 spells an accelerator by prefixing its letter with `&`, and `&&` for
/// a literal ampersand. AppKit has no equivalent, so the marker is stripped
/// for display and the position reported separately for underlining.
///
/// @param out_accel Receives the accelerator character, or 0 if none.
/// @param out_range Receives its range in the returned string.
[[nodiscard]] NSString* strip_win_accelerator(NSString* s,
                                              unichar* out_accel = nullptr,
                                              NSRange* out_range = nullptr);

/// The NSTextView inside a scroll-view-backed edit or list control, or nil.
[[nodiscard]] NSTextView* vtr_document_text_view(NSView* view);

/// Top edge of the union of all screens, in Cocoa's bottom-left-origin space.
///
/// The origin Win32 coordinates are measured down from. Input needs it to
/// place a cursor position, display needs it to describe monitor rectangles,
/// and both must use the same value or the two disagree by the height of the
/// display arrangement.
[[nodiscard]] CGFloat cocoa_virtual_top_y();

/// Draw the Win32 accelerator underline on a control's title.
///
/// Win32 marks the accelerator with an ampersand in the label and expects the
/// following character to be underlined; AppKit has no such convention, so the
/// underline is applied as an attributed-string range after the ampersand has
/// been stripped for display. `range` is what strip_win_accelerator reported.
void underline_win_accelerator(NSControl* control, NSRange range);

} // namespace vitreo::windowing
