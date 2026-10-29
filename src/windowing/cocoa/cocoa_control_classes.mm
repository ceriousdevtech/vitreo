// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_control_classes.mm
/// @brief Creating the AppKit view behind each standard Win32 control class.
///
/// The child-control container, and the per-class construction: BUTTON becomes
/// an NSButton, EDIT an NSTextField or a scroll-view-wrapped NSTextView,
/// STATIC an NSTextField that never accepts focus, and so on.
///
/// A Win32 control is a window: it has an HWND, a parent, a style word, and it
/// receives messages. An AppKit control is a view in a hierarchy with no such
/// identity. Everything here exists to give the second the first's shape —
/// which is why control creation is its own file rather than part of window
/// creation, even though on Windows they are the same call.
///
/// Split out of cocoa_bridge.mm. Contiguous, and closed except for
/// underline_win_accelerator, now declared in objc_helpers.h.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CATransaction.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAShapeLayer.h>
#import <objc/runtime.h>
#include "windowing/cocoa/objc_helpers.h"
#include "windowing/cocoa/cocoa_control_delegates.h"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/cocoa/app_icon_presentation.hpp"
#include "windowing/model/diag_flags.hpp"
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <algorithm>
#include <CoreFoundation/CFRunLoop.h>
#include <CoreGraphics/CGEventSource.h>
#include <mach/mach_time.h>

namespace vitreo::windowing {

namespace {
/// WS_EX_TRANSPARENT (WinUser.h). See CocoaBridge::m_transparent_children.
constexpr u32 kWsExTransparentChild = 0x00000020u;
} // namespace

// =============================================================================
// Child Control Views
// =============================================================================

bool CocoaBridge::rebind_child_control(HWND hwnd, const WindowData& data, u16 ordinal,
                                       const std::wstring& base_class, const std::wstring& text) {
    if (![NSThread isMainThread]) {
        __block bool result = false;
        const auto copy = data;
        const auto name_copy = base_class;
        const auto text_copy = text;
        vtr_run_on_main_sync(^{ result = rebind_child_control(hwnd, copy, ordinal, name_copy, text_copy); });
        return result;
    }
    auto it = m_controls.find(hwnd);
    if (it == m_controls.end() || !it->second) return false;
    NSView* old_view = (__bridge NSView*)it->second;
    NSView* parent = [old_view superview];
    NSArray<NSView*>* children = [[old_view subviews] copy];
    const BOOL hidden = [old_view isHidden];
    NSArray<NSView*>* siblings = [[parent subviews] copy];
    const NSUInteger index = [siblings indexOfObjectIdenticalTo:old_view];
    NSView* below = index != NSNotFound && index > 0 ? siblings[index - 1] : nil;
    add_child_view(data.parent, hwnd, data, ordinal, base_class, text);
    auto replacement = m_controls.find(hwnd);
    if (replacement == m_controls.end() || replacement->second == (__bridge void*)old_view) return false;
    NSView* view = (__bridge NSView*)replacement->second;
    for (NSView* child in children) {
        NSRect frame = [child frame];
        if ([old_view isFlipped] != [view isFlipped])
            frame.origin.y = NSHeight([view bounds]) - NSMaxY(frame);
        [child removeFromSuperview];
        [view addSubview:child];
        [child setFrame:frame];
    }
    [view setHidden:hidden];
    // The replaced view leaves the hierarchy: left in place it keeps drawing
    // its old presentation (text, font, pixels) under the replacement.
    [old_view removeFromSuperview];
    if (parent) [parent addSubview:view positioned:below ? NSWindowAbove : NSWindowBelow relativeTo:below];
    // add_child_view normally appends a tab stop. A rebind occupies its
    // existing slot, so rebuild the ring rather than using append-only links.
    const auto order = m_tab_order.find(data.parent);
    if (order != m_tab_order.end()) {
        std::vector<NSView*> tab_views;
        for (HWND item : order->second) {
            auto found = m_controls.find(item);
            if (found != m_controls.end() && found->second)
                tab_views.push_back((__bridge NSView*)found->second);
        }
        for (size_t i = 0; i < tab_views.size(); ++i)
            [tab_views[i] setNextKeyView:tab_views[(i + 1) % tab_views.size()]];
    }
    // The old custom class's pixel backing no longer presents this HWND.
    // Keep all HWND/control/capture bookkeeping and descendant views alive.
    m_guest_pixel_views.erase(hwnd);
    m_presentation_views.erase(hwnd);
    return true;
}

void CocoaBridge::add_child_view(HWND parent_hwnd, HWND child_hwnd,
                                  const WindowData& data,
                                  u16 class_ordinal, const std::wstring& class_name,
                                  const std::wstring& text) {
    if (![NSThread isMainThread]) {
        WindowData data_copy = data;
        std::wstring class_name_copy = class_name;
        std::wstring text_copy = text;
        vtr_run_on_main_sync(^{
            this->add_child_view(parent_hwnd, child_hwnd, data_copy, class_ordinal, class_name_copy, text_copy);
        });
        return;
    }

    NSView* content = nil;
    NativeWindowState* top_state = nullptr;
    NSWindow* grow_window = nil;  // non-nil when parent is a top-level window we may need to grow

    auto win_it = m_windows.find(parent_hwnd);
    if (win_it != m_windows.end() && win_it->second.ns_window) {
        NSWindow* parent_win = reinterpret_cast<NSWindow*>(win_it->second.ns_window);
        content = [parent_win contentView];
        top_state = &win_it->second;
        if (win_it->second.is_dialog) {
            grow_window = parent_win;
        }
    } else {
        // Parent may be a container view/control (WS_CHILD dialog)
        auto ctrl_it = m_controls.find(parent_hwnd);
        if (ctrl_it != m_controls.end()) {
            NSView* parent_view = (__bridge NSView*)ctrl_it->second;
            if ([parent_view isKindOfClass:[NSBox class]]) {
                content = [(NSBox*)parent_view contentView];
            } else {
                content = parent_view;
            }
            // Attempt to locate the owning NSWindow so we can keep handlers alive
            NSWindow* owner = [content window];
            if (owner) {
                HWND owner_hwnd = find_hwnd_for_window((__bridge void*)owner);
                auto owner_it = m_windows.find(owner_hwnd);
                if (owner_it != m_windows.end()) {
                    top_state = &owner_it->second;
                }
            }
        }
    }

    if (!content) {
        VITREO_TRACE( "[cocoa] add_child_view: no native parent for 0x%x\n",
                     parent_hwnd);
        return;
    }

    // Convert Windows coordinates (Y-down from top of client) to Cocoa.
    // If the parent view is flipped (VtrContentView), Y is already top-down.
    // Otherwise convert from Y-up (e.g. NSBox contentView).
    CGFloat content_h = [content bounds].size.height;
    CGFloat x  = static_cast<CGFloat>(data.rect.left);
    CGFloat w  = static_cast<CGFloat>(data.rect.right  - data.rect.left);
    CGFloat h  = static_cast<CGFloat>(data.rect.bottom - data.rect.top);
    CGFloat y;
    if ([content isFlipped]) {
        y = static_cast<CGFloat>(data.rect.top);
    } else {
        // Bottom-relative even while the parent is still 0 high, so the
        // shift set_control_frame applies when the parent grows lands the
        // child at its Win32 top offset.
        y = content_h - static_cast<CGFloat>(data.rect.top) - h;
    }

    NSRect frame = NSMakeRect(x, y, w, h);
    unichar accel_char = 0;
    NSRange accel_range = NSMakeRange(NSNotFound, 0);
    NSString* label = strip_win_accelerator(to_nsstring(text), &accel_char, &accel_range);

    // Win32 WS_GROUP marks the first control of a new group; controls between
    // two WS_GROUP boundaries belong to one group. Track this in creation order
    // so auto-radio buttons can be grouped per Win32 semantics (not macOS's
    // superview-wide auto-grouping).
    constexpr u32 WS_GROUP_STYLE = 0x00020000;
    if (data.style & WS_GROUP_STYLE) {
        begin_radio_group();
    }

    NSView* view = nil;
    // For radios: the per-WS_GROUP-group action selector (set in the radio case).
    NSString* radio_action_sel = nil;

    // --- Standard control class ordinals ---
    // 0x0080 = Button, 0x0081 = Edit, 0x0082 = Static,
    // 0x0083 = ListBox, 0x0084 = ScrollBar, 0x0085 = ComboBox

    // Extract control ID — dialog templates stash it in user_data; CreateWindowEx
    // children store it in ctrl_id (hMenu). Prefer ctrl_id when set.
    u32 control_id = data.ctrl_id != 0 ? data.ctrl_id : static_cast<u32>(data.user_data);

    static const char kVtrButtonHandlerKey = 0;

    switch (class_ordinal) {
        case 0x0080: { // Button
            u32 btn_type = data.style & 0x0F;
            NSButton* btn = nil;

            if (btn_type == 0x07) {
                // BS_GROUPBOX → NSBox. NSBoxCustom positions the title correctly in
                // our flipped content view (NSBoxPrimary draws it ~20px too high,
                // overlapping adjacent controls). Use a subtle, rounded native
                // border (separator color) rather than a hard solid line.
                NSBox* box = [[NSBox alloc] initWithFrame:frame];
                [box setBoxType:NSBoxCustom];
                [box setBorderType:NSLineBorder];
                [box setBorderWidth:1.0];
                [box setBorderColor:[NSColor separatorColor]];
                [box setCornerRadius:5.0];
                [box setFillColor:[NSColor clearColor]];
                [box setTitle:label];
                [box setTitlePosition:label.length ? NSAtTop : NSNoTitle];
                [box setTitleFont:[NSFont systemFontOfSize:11.0]];
                view = box;
            } else if (btn_type == 0x02 || btn_type == 0x03 || btn_type == 0x05 || btn_type == 0x06) {
                // BS_CHECKBOX / BS_AUTOCHECKBOX / BS_3STATE / BS_AUTO3STATE →
                // NSButton checkbox; the 3-state types also show
                // BST_INDETERMINATE (Learn: Button Styles). VCL's TCheckBox
                // is BS_3STATE.
                btn = [[NSButton alloc] initWithFrame:frame];
                [btn setButtonType:NSButtonTypeSwitch];
                if (btn_type == 0x05 || btn_type == 0x06) [btn setAllowsMixedState:YES];
                [btn setTitle:label];
                [btn setBezelStyle:NSBezelStyleRegularSquare];
                if (accel_char != 0) {
                    [btn setKeyEquivalent:[NSString stringWithFormat:@"%C", accel_char]];
                    [btn setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
                }
                view = btn;
            } else if (btn_type == 0x04 || btn_type == 0x09) {
                // BS_RADIOBUTTON / BS_AUTORADIOBUTTON
                btn = [[NSButton alloc] initWithFrame:frame];
                [btn setButtonType:NSButtonTypeRadio];
                [btn setTitle:label];
                if (accel_char != 0) {
                    [btn setKeyEquivalent:[NSString stringWithFormat:@"%C", accel_char]];
                    [btn setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
                }
                view = btn;
                // Pick a distinct click selector for this radio's WS_GROUP group
                // so macOS treats it as an independent native radio group.
                if (m_active_radio_group == 0) m_active_radio_group = ++m_radio_group_seq;
                radio_action_sel = [NSString stringWithFormat:@"radioClick%u:",
                                    static_cast<unsigned>(m_active_radio_group % 16u)];
            } else {
                // BS_PUSHBUTTON / BS_DEFPUSHBUTTON / other
                btn = [[NSButton alloc] initWithFrame:frame];
                [btn setButtonType:NSButtonTypeMomentaryPushIn];
                [btn setBezelStyle:NSBezelStyleRounded];
                [btn setTitle:label];
                if (btn_type == 0x0B) {
                    // BS_OWNERDRAW: the owner draws the whole button in
                    // WM_DRAWITEM (Learn: Button Styles), so it has no native
                    // look; it still clicks (BN_CLICKED) and hosts children.
                    [btn setTransparent:YES];
                }
                if (btn_type == 0x01) {
                    // BS_DEFPUSHBUTTON — make it the default key equivalent
                    [btn setKeyEquivalent:@"\r"];
                } else if (accel_char != 0) {
                    // Windows Alt+accel → macOS Cmd+accel
                    [btn setKeyEquivalent:[NSString stringWithFormat:@"%C", accel_char]];
                    [btn setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
                }
                view = btn;
            }

            // Wire up button action handler (if it's a real button, not groupbox)
            if (btn != nil) {
                VtrButtonHandler* handler = [[VtrButtonHandler alloc] init];
                handler.parentHwnd = parent_hwnd;
                handler.controlHwnd = child_hwnd;
                handler.controlId  = control_id;
                handler.queue      = m_queue;  // May be nil initially, updated later
                handler.bridge     = this;
                [btn setTarget:handler];
                // Radios use a per-WS_GROUP-group selector so macOS keeps the
                // groups independent; all other buttons share buttonClicked:.
                [btn setAction:(radio_action_sel ? NSSelectorFromString(radio_action_sel)
                                                 : @selector(buttonClicked:))];

                // Keep handler alive for the lifetime of the button view
                objc_setAssociatedObject(btn, &kVtrButtonHandlerKey, handler,
                                         OBJC_ASSOCIATION_RETAIN_NONATOMIC);
                // Also store on the owning top-level window state if available (debugging/cleanup)
                if (top_state) {
                    top_state->button_handlers.push_back((__bridge void*)handler);
                }
            }
            break;
        }

        case 0x0081: { // Edit
            if (data.style & 0x00000020) { // ES_PASSWORD
                NSSecureTextField* secure = [[NSSecureTextField alloc] initWithFrame:frame];
                [secure setStringValue:label];
                view = secure;
            } else if (data.style & 0x00000004) { // ES_MULTILINE
                NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:frame];
                [scroll setHasVerticalScroller:YES];
                [scroll setHasHorizontalScroller:NO];
                [scroll setBorderType:NSBezelBorder];
                [scroll setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

                NSSize contentSize = [scroll contentSize];
                NSTextView* textView = [[NSTextView alloc]
                    initWithFrame:NSMakeRect(0, 0, contentSize.width, contentSize.height)];
                [textView setEditable:YES];
                [textView setSelectable:YES];
                [textView setRichText:NO];
                [textView setHorizontallyResizable:NO];
                [textView setVerticallyResizable:YES];
                [textView setString:label ?: @""];
                [[textView textContainer] setWidthTracksTextView:YES];
                [[textView textContainer] setContainerSize:NSMakeSize(contentSize.width, CGFLOAT_MAX)];
                [textView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

                [scroll setDocumentView:textView];
                view = scroll;
            } else {
                NSTextField* field = [[NSTextField alloc] initWithFrame:frame];
                [field setStringValue:label];
                [field setEditable:YES];
                [field setBezeled:YES];
                [field setDrawsBackground:YES];
                [field setBezelStyle:NSTextFieldSquareBezel];
                view = field;
            }
            // Wire EN_CHANGE for single-line / secure fields (NSSecureTextField is
            // an NSTextField subclass) so the guest is notified as the user types.
            if ([view isKindOfClass:[NSTextField class]]) {
                VtrEditHandler* eh = [[VtrEditHandler alloc] init];
                eh.parentHwnd  = parent_hwnd;
                eh.controlHwnd = child_hwnd;
                eh.controlId   = control_id;
                eh.bridge      = this;
                [(NSTextField*)view setDelegate:(id<NSTextFieldDelegate>)eh];
                static const char kVtrEditHandlerKey = 0;
                objc_setAssociatedObject(view, &kVtrEditHandlerKey, eh,
                                         OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            }
            break;
        }

        case 0x0082: { // Static
            u32 ss_type = data.style & 0x1F;
            if (ss_type == 0x10 || ss_type == 0x11 || ss_type == 0x12) {
                // SS_ETCHEDHORZ / SS_ETCHEDVERT / SS_ETCHEDFRAME
                NSBox* box = [[NSBox alloc] initWithFrame:frame];
                [box setBoxType:NSBoxSeparator];
                view = box;
            } else if (ss_type == 0x04 || ss_type == 0x05 || ss_type == 0x06
                    || ss_type == 0x07 || ss_type == 0x08 || ss_type == 0x09) {
                // SS_BLACKRECT / SS_GRAYRECT / SS_WHITERECT / SS_*FRAME.
                // Installer wizards use these styles as an embedding panel
                // (control id 1018 is a common one).
                // Use a plain NSView to avoid NSBox content insets affecting layout.
                // Do NOT use setWantsLayer:YES — a layer-backed panel with default
                // contentsScale=1.0 can cause all subviews to render at 1x on Retina,
                // producing blurry text and images.
                NSView* panel = [[NSView alloc] initWithFrame:frame];
                view = panel;
            } else if (ss_type == 0x0E || ss_type == 0x03) {
                // SS_BITMAP (0x0E) / SS_ICON (0x03)
                // MS Learn: SS_REALSIZECONTROL stretches independently to the
                // control (aspect may change); with SS_CENTERIMAGE, center+clip.
                // Without REALSIZECONTROL, the bitmap is not aspect-fit letterboxed
                // into the control — place at natural size (top-left).
                constexpr u32 kSsRealSizeControl = 0x0040;
                constexpr u32 kSsCenterImage = 0x0200;
                NSImageView* iv = [[NSImageView alloc] initWithFrame:frame];
                if ((data.style & kSsRealSizeControl) != 0) {
                    if ((data.style & kSsCenterImage) != 0) {
                        [iv setImageScaling:NSImageScaleNone];
                        [iv setImageAlignment:NSImageAlignCenter];
                    } else {
                        [iv setImageScaling:NSImageScaleAxesIndependently];
                    }
                } else {
                    [iv setImageScaling:NSImageScaleNone];
                    [iv setImageAlignment:NSImageAlignTopLeft];
                }
                [iv setImage:nil];
                view = iv;
            } else {
                NSTextField* tf = [[NSTextField alloc] initWithFrame:frame];
                [tf setStringValue:label];
                [tf setEditable:NO];
                [tf setSelectable:NO];
                // Win32 STATIC labels never take keyboard focus. Without this,
                // AppKit lets the label steal first responder from a nearby
                // EDIT (a wizard page places a prompt label just above the
                // text field) so clicks and typing feel locked until the user
                // re-focuses the edit.
                [tf setRefusesFirstResponder:YES];
                [tf setBordered:NO];
                [tf setDrawsBackground:NO];
                if (ss_type != 0x0B && ss_type != 0x0C) {
                    [tf setLineBreakMode:NSLineBreakByWordWrapping];
                    [tf setUsesSingleLineMode:NO];
                    [tf setMaximumNumberOfLines:0];
                }
                NSSize text_size = [label sizeWithAttributes:@{
                    NSFontAttributeName: [tf font]
                }];
                const CGFloat required_width = std::ceil(text_size.width) + 2.0;
                if ([label rangeOfString:@"\n"].location == NSNotFound &&
                    required_width > frame.size.width &&
                    required_width <= frame.size.width + 12.0) {
                    NSRect adjusted_frame = [tf frame];
                    adjusted_frame.size.width = required_width;
                    [tf setFrame:adjusted_frame];
                }
                // Alignment
                if (ss_type == 0x01) {
                    [tf setAlignment:NSTextAlignmentCenter];
                } else if (ss_type == 0x02) {
                    [tf setAlignment:NSTextAlignmentRight];
                } else {
                    [tf setAlignment:NSTextAlignmentLeft];
                }
                view = tf;
            }
            break;
        }

        case 0x0083: { // ListBox → NSScrollView + NSTableView (single column)
            if ((data.style & 0x30) == 0x10 && !(data.style & 0x40)) {
                // Owner-drawn, no-string list boxes are guest-painted surfaces.
                // USER32 supplies WM_MEASUREITEM / WM_DRAWITEM, not native labels.
                view = [[NSClassFromString(@"VtrContentView") alloc] initWithFrame:frame];
                break;
            }

            NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:frame];
            [scroll setHasVerticalScroller:YES];
            [scroll setBorderType:NSBezelBorder];
            [scroll setAutohidesScrollers:YES];

            NSTableView* table = [[NSTableView alloc]
                                  initWithFrame:[[scroll contentView] bounds]];
            NSTableColumn* column = [[NSTableColumn alloc] initWithIdentifier:@"item"];
            [column setEditable:NO];
            [column setWidth:[[scroll contentView] bounds].size.width];
            [table addTableColumn:column];
            [table setHeaderView:nil];
            [table setRowSizeStyle:NSTableViewRowSizeStyleSmall];
            [table setAllowsEmptySelection:YES];
            [table setAllowsMultipleSelection:NO];
            [table setFocusRingType:NSFocusRingTypeNone];

            VtrListBoxController* lbc = [[VtrListBoxController alloc] init];
            lbc.bridge = this;
            lbc.hwnd   = child_hwnd;
            lbc.parent = parent_hwnd;
            lbc.ctrlId = control_id;
            lbc.queue  = m_queue;
            [table setDataSource:lbc];
            [table setDelegate:lbc];
            [table setTarget:lbc];
            [table setDoubleAction:@selector(doubleClicked:)];

            static const char kVtrListBoxControllerKey = 0;
            objc_setAssociatedObject(table, &kVtrListBoxControllerKey, lbc,
                                     OBJC_ASSOCIATION_RETAIN_NONATOMIC);

            [scroll setDocumentView:table];
            view = scroll;

            // Backing storage for LB_* query semantics (the data source reads it).
            m_listbox_items[child_hwnd] = {};
            m_listbox_selection[child_hwnd] = -1;
            break;
        }

        case 0x0084: { // ScrollBar → NSScroller
            NSScroller* scroller = [[NSScroller alloc] initWithFrame:frame];
            [scroller setEnabled:YES];
            view = scroller;
            break;
        }

        case 0x0085: { // ComboBox → NSPopUpButton
            // A Win32 combobox rect spans the closed control PLUS the dropped-down
            // list, so it is tall; the closed control occupies only the top. An
            // NSPopUpButton fills its whole frame, which would make it oversized
            // and overlap the controls below. Pin it to a standard control height
            // at the top of the Win32 rect (the content view is flipped, so the
            // origin is already the top).
            NSRect cf = frame;
            const CGFloat kComboH = 22.0;
            if (cf.size.height > kComboH) cf.size.height = kComboH;
            if ((data.style & 3u) == 2u) { // CBS_DROPDOWN has an edit field.
                NSComboBox* combo = [[NSComboBox alloc] initWithFrame:cf];
                [combo setEditable:YES];
                [combo setStringValue:label];
                VtrEditHandler* handler = [[VtrEditHandler alloc] init];
                handler.parentHwnd = parent_hwnd;
                handler.controlHwnd = child_hwnd;
                handler.controlId = control_id;
                handler.bridge = this;
                [combo setDelegate:handler];
                static const char kComboHandlerKey = 0;
                objc_setAssociatedObject(combo, &kComboHandlerKey, handler,
                                         OBJC_ASSOCIATION_RETAIN_NONATOMIC);
                m_combo_item_data[child_hwnd] = {};
                view = combo;
                break;
            }
            NSPopUpButton* popup = [[NSPopUpButton alloc] initWithFrame:cf pullsDown:NO];
            if ([label length] > 0) {
                [popup addItemWithTitle:label];
            }
            view = popup;
            break;
        }

        default: {
            // Check custom class names when ordinal is 0
            if (class_ordinal == 0 && !class_name.empty()) {
                // Case-insensitive comparison for custom class names
                std::wstring lower_name = class_name;
                for (auto& ch : lower_name) {
                    ch = towlower(ch);
                }

                if (lower_name == L"tooltips_class32") {
                    // The bubble is presented natively; the control's own view
                    // must stay empty and invisible rather than showing as a
                    // blank white rectangle.
                    view = [[NSView alloc] initWithFrame:NSZeroRect];
                    [view setHidden:YES];
                } else if (lower_name == L"rebarwindow32") {
                    view = [[NSView alloc] initWithFrame:frame];
                } else if (lower_name == L"toolbarwindow32") {
                    view = [[NSView alloc] initWithFrame:frame];
                    m_toolbars[child_hwnd] = {};
                } else if (lower_name == L"msctls_statusbar32") {
                    view = [[NSView alloc] initWithFrame:frame];
                    m_status_parts[child_hwnd] = {{-1}, {L""}};
                } else if (lower_name == L"msctls_progress32") {
                    // Progress bar → NSProgressIndicator
                    NSProgressIndicator* progress = [[NSProgressIndicator alloc] initWithFrame:frame];
                    [progress setStyle:NSProgressIndicatorStyleBar];
                    [progress setIndeterminate:NO];
                    [progress setMinValue:0.0];
                    [progress setMaxValue:100.0];
                    [progress setDoubleValue:0.0];
                    view = progress;
                } else if (lower_name == L"syslink") {
                    // SysLink: the markup's initial items, all LIS_ENABLED --
                    // the same state user32's SysLink model starts from.
                    VtrSysLinkView* link = [[VtrSysLinkView alloc] initWithFrame:frame];
                    const auto markup = syslink::parse_markup(
                        text, (data.style & syslink::LWS_NOPREFIX) == 0);
                    const std::vector<u32> states(markup.items.size(),
                                                  syslink::LIS_ENABLED);
                    vtr_syslink_apply(link, markup, states, data.style);
                    view = link;
                } else if (lower_name == L"richedit20a" ||
                           lower_name == L"richedit20w" ||
                           lower_name == L"richedit" ||
                           lower_name == L"richedit50w") {
                    // RichEdit → multiline NSScrollView + NSTextView (EULA etc.)
                    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:frame];
                    [scroll setHasVerticalScroller:YES];
                    [scroll setHasHorizontalScroller:NO];
                    [scroll setBorderType:NSBezelBorder];
                    [scroll setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
                    [[scroll contentView] setPostsBoundsChangedNotifications:YES];

                    NSSize contentSize = [scroll contentSize];
                    NSTextView* textView = [[NSTextView alloc]
                        initWithFrame:NSMakeRect(0, 0, contentSize.width, contentSize.height)];
                    const bool readonly = (data.style & 0x00000800) != 0; // ES_READONLY
                    [textView setEditable:!readonly];
                    [textView setSelectable:YES];
                    [textView setRichText:YES];
                    [textView setTextColor:[NSColor textColor]];
                    [textView setBackgroundColor:[NSColor textBackgroundColor]];
                    [textView setInsertionPointColor:[NSColor textColor]];
                    [textView setHorizontallyResizable:NO];
                    [textView setVerticallyResizable:YES];
                    [textView setString:label ?: @""];
                    [[textView textContainer] setWidthTracksTextView:YES];
                    [[textView textContainer] setContainerSize:NSMakeSize(contentSize.width, CGFLOAT_MAX)];
                    [textView setAutoresizingMask:NSViewWidthSizable];
                    [scroll setDocumentView:textView];
                    view = scroll;

                    VtrRichEditScrollHandler* sh = [[VtrRichEditScrollHandler alloc] init];
                    sh.parentHwnd = parent_hwnd;
                    sh.controlHwnd = child_hwnd;
                    sh.controlId = control_id;
                    sh.bridge = this;
                    [[NSNotificationCenter defaultCenter]
                        addObserver:sh
                           selector:@selector(boundsDidChange:)
                               name:NSViewBoundsDidChangeNotification
                             object:[scroll contentView]];
                    static const char kVtrRichScrollKey = 0;
                    objc_setAssociatedObject(scroll, &kVtrRichScrollKey, sh,
                                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);

                    VITREO_TRACE(
                        "[cocoa] add_child_view: RichEdit → NSScrollView+NSTextView\n");
                } else if (lower_name == L"syslistview32") {
                    // ListView → NSScrollView + NSTextView (log-style)
                    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:frame];
                    [scroll setHasVerticalScroller:YES];
                    [scroll setHasHorizontalScroller:NO];
                    [scroll setBorderType:NSBezelBorder];
                    [scroll setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

                    NSSize contentSize = [scroll contentSize];
                    NSTextView* textView = [[NSTextView alloc]
                        initWithFrame:NSMakeRect(0, 0, contentSize.width, contentSize.height)];
                    [textView setEditable:NO];
                    [textView setSelectable:YES];
                    [textView setRichText:NO];
                    [textView setFont:[NSFont monospacedSystemFontOfSize:11.0 weight:NSFontWeightRegular]];
                    [textView setTextColor:[NSColor textColor]];
                    [textView setBackgroundColor:[NSColor textBackgroundColor]];
                    [textView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
                    [[textView textContainer] setWidthTracksTextView:YES];

                    [scroll setDocumentView:textView];
                    view = scroll;

                    // Initialize empty item storage for this listview
                    m_listview_items[child_hwnd] = {};

                    VITREO_TRACE( "[cocoa] add_child_view: SysListView32 → NSScrollView+NSTextView\n");
                } else if (lower_name == L"systabcontrol32") {
                    // Win32-style left-aligned strip. NSTabView's centered
                    // macOS tabs covered sibling editors (Scintilla).
                    VtrTabStripView* tabs =
                        [[VtrTabStripView alloc] initWithFrame:frame];
                    tabs.bridge = this;
                    tabs.hwnd = child_hwnd;
                    tabs.suppressNotify = NO;
                    view = tabs;

                    TabControlData tab_data;
                    tab_data.tab_view = (__bridge void*)tabs;
                    tab_data.parent = parent_hwnd;
                    tab_data.ctrl_id = control_id;
                    tab_data.selected = 0;
                    m_tab_controls[child_hwnd] = std::move(tab_data);

                    VITREO_TRACE(
                        "[cocoa] add_child_view: SysTabControl32 → VtrTabStripView\n");
                } else if (lower_name == L"systreeview32") {
                    // TreeView → NSScrollView + NSOutlineView (hierarchical).
                    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:frame];
                    [scroll setHasVerticalScroller:YES];
                    [scroll setHasHorizontalScroller:NO];
                    [scroll setBorderType:NSBezelBorder];
                    [scroll setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

                    NSOutlineView* outline =
                        [[NSOutlineView alloc] initWithFrame:[scroll bounds]];
                    NSTableColumn* col =
                        [[NSTableColumn alloc] initWithIdentifier:@"VtrTreeCol"];
                    [col setEditable:NO];
                    [col setWidth:[scroll contentSize].width];
                    [col setMinWidth:1.0];
                    [outline addTableColumn:col];
                    [outline setOutlineTableColumn:col];
                    [outline setHeaderView:nil];
                    [outline setColumnAutoresizingStyle:NSTableViewLastColumnOnlyAutoresizingStyle];
                    [outline setIndentationPerLevel:16.0];
                    [outline setRowHeight:22.0];
                    [outline setIntercellSpacing:NSMakeSize(0.0, 2.0)];
                    [outline setSelectionHighlightStyle:NSTableViewSelectionHighlightStyleRegular];
                    [outline setBackgroundColor:[NSColor controlBackgroundColor]];
                    if (@available(macOS 11.0, *)) {
                        [outline setStyle:NSTableViewStylePlain];
                    }
                    [outline setAllowsEmptySelection:YES];

                    VtrTreeController* ctl = [[VtrTreeController alloc] init];
                    ctl.bridge  = this;
                    ctl.hwnd    = child_hwnd;
                    ctl.outline = outline;
                    [outline setDataSource:ctl];
                    [outline setDelegate:ctl];
                    // dataSource/delegate are weak (non-ARC) — keep the controller
                    // alive for the outline view's lifetime.
                    static const char kVtrTreeControllerKey = 0;
                    objc_setAssociatedObject(outline, &kVtrTreeControllerKey, ctl,
                                             OBJC_ASSOCIATION_RETAIN);

                    [scroll setDocumentView:outline];
                    view = scroll;

                    TreeViewData tv;
                    tv.parent     = parent_hwnd;
                    tv.ctrl_id    = control_id;
                    tv.outline    = (__bridge void*)outline;
                    tv.controller = (__bridge void*)ctl;
                    m_treeviews[child_hwnd] = std::move(tv);

                    VITREO_TRACE( "[cocoa] add_child_view: SysTreeView32 → NSOutlineView (id=%u)\n",
                                 control_id);
                } else if (data.wndproc != 0) {
                    // App-registered custom class with its own wndproc (e.g.
                    // Solitaire's "Stat" status bar).  Windows lets the app paint
                    // this child's client area itself via WM_PAINT/GDI.  Give it a
                    // VtrPixelView so its per-hwnd framebuffer (flushed by
                    // EndPaint → update_window_pixels) is actually composited on
                    // screen, rather than an opaque placeholder that would hide it.
                    //
                    // Size the view to the CLIENT rect, not the outer rect: the
                    // per-hwnd framebuffer is client-sized, and VtrPixelView
                    // presents top-down with kCAGravityTopLeft. A larger view
                    // would pin a smaller blit to the top-left instead of
                    // filling the control.
                    CGFloat cw = static_cast<CGFloat>(data.client_rect.right - data.client_rect.left);
                    CGFloat ch = static_cast<CGFloat>(data.client_rect.bottom - data.client_rect.top);
                    NSRect pv_frame = NSMakeRect(x, y,
                                                 cw > 0 ? cw : w,
                                                 ch > 0 ? ch : h);
                    VtrPixelView* pv = [[VtrPixelView alloc] initWithFrame:pv_frame];
                    // Billboard children of a skinned host BitBlt a full
                    // client-sized bitmap. Fill the view and
                    // keep a clear layer so the borderless skin host does not
                    // show a black/white plate around the splash.
                    pv.scalesFramebuffer = NO;
                    pv.layer.contentsGravity = kCAGravityResize;
                    pv.layer.backgroundColor = [NSColor clearColor].CGColor;
                    view = pv;

                    VITREO_TRACE( "[cocoa] add_child_view: app-painted custom class \"%ls\" → VtrPixelView\n",
                                 class_name.c_str());
                } else {
                    // Unknown custom class with no guest wndproc — render as
                    // bordered placeholder.
                    NSBox* placeholder = [[NSBox alloc] initWithFrame:frame];
                    [placeholder setTitle:label];
                    [placeholder setBoxType:NSBoxCustom];
                    [placeholder setBorderType:NSLineBorder];
                    view = placeholder;

                    VITREO_TRACE( "[cocoa] add_child_view: unknown custom class \"%ls\"\n",
                                 class_name.c_str());
                }
            } else {
                // Unknown ordinal — render as bordered placeholder
                NSBox* placeholder = [[NSBox alloc] initWithFrame:frame];
                [placeholder setTitle:label];
                [placeholder setBoxType:NSBoxCustom];
                [placeholder setBorderType:NSLineBorder];
                view = placeholder;
            }
            break;
        }
    }

    if (view) {
        [view setAutoresizingMask:NSViewNotSizable];

        // If a view for this HWND already exists (e.g. dialog template control
        // being replaced by a CreateWindowExW call), remove the stale view first
        // to avoid duplicate NSViews at the same position.
        auto existing_it = m_controls.find(child_hwnd);
        if (existing_it != m_controls.end() && existing_it->second != nullptr) {
            NSView* old_view = (__bridge NSView*)existing_it->second;
            [old_view removeFromSuperview];
        }

        // Pin every control to the top-left of the (flipped) content view. The
        // window grow-to-fit resizes the content view after these controls exist;
        // NSButton's default autoresizing mask otherwise slides radios/checkboxes
        // down by the grow delta (NSTextField/NSPopUpButton keep their position).
        // NSViewMaxYMargin keeps the distance from the TOP fixed in a flipped view.
        [view setAutoresizingMask:(NSViewMaxXMargin | NSViewMaxYMargin)];

        // Group boxes should sit behind the controls they visually contain, but
        // ABOVE the opaque framebuffer pixel view (which lives at the very bottom
        // of the z-order). Adding them with relativeTo:nil put them below the
        // pixel view, so the box border + title rendered behind the background and
        // were invisible. Position just above the pixel view instead.
        if (class_ordinal == 0x0080 && ((data.style & 0x0F) == 0x07)) {
            VtrPixelView* pv = nil;
            for (NSView* sub in [content subviews]) {
                if ([sub isKindOfClass:[VtrPixelView class]]) { pv = (VtrPixelView*)sub; break; }
            }
            if (pv) {
                [content addSubview:view positioned:NSWindowAbove relativeTo:pv];
            } else {
                [content addSubview:view positioned:NSWindowBelow relativeTo:nil];
            }
        } else {
            [content addSubview:view];
        }

        // Grow the dialog window downward if this *visible* control extends past
        // the current client area. A dialog computes control pixel positions with
        // its own DLU base units, which can exceed Vitreo's template-derived
        // client height, so the bottom buttons get clipped.
        //
        // Use the native view frame (not the raw Win32 template rect): ComboBox
        // templates encode closed+dropdown height (e.g. 607px) which must not
        // grow the dialog (Language picker became a tall white pillar). Skip
        // invisible controls. A wizard hides 640×480 skin hosts and sentinel
        // "C" buttons past the client bottom that were inflating later pages
        // with empty letterbox space.
        if (grow_window && data.visible && view) {
            const NSRect grow_frame = [view frame];
            CGFloat needed = grow_frame.origin.y + grow_frame.size.height + 8.0;
            CGFloat have   = [content bounds].size.height;
            if (needed > have + 0.5) {
                CGFloat delta = needed - have;
                NSRect wf = [grow_window frame];
                wf.origin.y    -= delta;
                wf.size.height += delta;
                // Disable subview autoresizing across the resize so existing
                // controls (notably NSButton radios/checkboxes, which AppKit drags
                // by the height delta) keep their absolute Win32 positions. Also
                // suppress the WM_SIZE this would post, so the guest doesn't
                // relayout its controls for a resize it never requested. Then
                // resize only the framebuffer pixel view to the new bounds.
                BOOL prevAuto = [content autoresizesSubviews];
                [content setAutoresizesSubviews:NO];
                set_suppress_resize_notify(true);
                [grow_window setFrame:wf display:YES];
                set_suppress_resize_notify(false);
                [content setAutoresizesSubviews:prevAuto];
                for (NSView* sub in [content subviews]) {
                    if ([sub isKindOfClass:[VtrPixelView class]]) {
                        [sub setFrame:[content bounds]];
                        break;
                    }
                }
            }
        }

        // Respect WS_VISIBLE: hide controls that are not initially visible
        if (!data.visible) {
            [view setHidden:YES];
            // A control created without WS_VISIBLE is just as much a
            // guest-expressed hide as a later ShowWindow(SW_HIDE), so it has to
            // join the same set. Otherwise the sweep in
            // sync_native_window_visibility — which un-hides every subview the
            // guest did not explicitly hide — resurrects it the first time the
            // host window is shown. Multi-page installers build every page's
            // controls up front and hidden, so missing this draws all pages
            // stacked on top of each other.
            m_controls_hidden_by_guest.insert(child_hwnd);
        }

        if (accel_range.location != NSNotFound &&
            ([view isKindOfClass:[NSButton class]] ||
             (class_ordinal == 0x0082 && [view isKindOfClass:[NSTextField class]]))) {
            underline_win_accelerator((NSControl*)view, accel_range);
        }

        // Store the view for later text updates (SetWindowText, SetDlgItemText)
        m_controls[child_hwnd] = (__bridge void*)view;
        if ((data.ex_style & kWsExTransparentChild) != 0) {
            m_transparent_children.insert(child_hwnd);
            restack_transparent_children((__bridge void*)[view superview]);
        }
        HWND owner_hwnd = top_state ? top_state->hwnd : parent_hwnd;
        const u32 radio_group =
            (class_ordinal == 0x0080 &&
             ((data.style & 0x0F) == 0x04 || (data.style & 0x0F) == 0x09))
                ? m_active_radio_group
                : 0u;
        m_control_state[child_hwnd] = {
            parent_hwnd, owner_hwnd, data.style, class_ordinal, control_id,
            radio_group};
        auto& control_order = m_control_order[parent_hwnd];
        if (std::find(control_order.begin(), control_order.end(), child_hwnd) == control_order.end()) {
            control_order.push_back(child_hwnd);
        }

        constexpr u32 WS_TABSTOP_STYLE = 0x00010000;
        if (data.style & WS_TABSTOP_STYLE) {
            auto& tab_order = m_tab_order[parent_hwnd];
            if (std::find(tab_order.begin(), tab_order.end(), child_hwnd) == tab_order.end()) {
                tab_order.push_back(child_hwnd);
            }
            if (tab_order.size() > 1) {
                NSView* previous = (__bridge NSView*)m_controls[tab_order[tab_order.size() - 2]];
                NSView* first = (__bridge NSView*)m_controls[tab_order.front()];
                [previous setNextKeyView:view];
                [view setNextKeyView:first];
            } else {
                [[view window] setInitialFirstResponder:view];
                [view setNextKeyView:view];
            }
            auto pending = m_pending_static_mnemonic.find(parent_hwnd);
            if (pending != m_pending_static_mnemonic.end()) {
                m_mnemonics[owner_hwnd][pending->second] = child_hwnd;
                m_pending_static_mnemonic.erase(pending);
            }
        }

        if (accel_char != 0) {
            u32 mnemonic = static_cast<u32>(towlower(accel_char));
            if (class_ordinal == 0x0080 && ((data.style & 0x0F) != 0x07)) {
                m_mnemonics[owner_hwnd][mnemonic] = child_hwnd;
            } else if (class_ordinal == 0x0082 && !(data.style & 0x00000080)) {
                m_pending_static_mnemonic[parent_hwnd] = mnemonic;
            }
        }

        if (diag::win_diag()) {
            NSRect vf = [view frame];
            NSRect wf = [view convertRect:[view bounds] toView:nil]; // window coords
            fprintf(stderr, "[align] hwnd=0x%x cls=0x%04x win=(%d,%d) superv=(%.0f,%.0f,%.0f,%.0f) windowXY=(%.0f,%.0f) flipped=%d '%s'\n",
                    child_hwnd, class_ordinal,
                    data.rect.left, data.rect.top,
                    vf.origin.x, vf.origin.y, vf.size.width, vf.size.height,
                    wf.origin.x, wf.origin.y,
                    [content isFlipped] ? 1 : 0,
                    [label UTF8String]);
        }

        VITREO_TRACE( "[cocoa] add_child_view: parent=0x%x child=0x%x "
                     "class=0x%04x frame=(%.0f,%.0f,%.0f,%.0f) visible=%s \"%s\"\n",
                     parent_hwnd, child_hwnd, class_ordinal,
                     frame.origin.x, frame.origin.y,
                     frame.size.width, frame.size.height,
                     data.visible ? "yes" : "no",
                     [label UTF8String]);
    }
}

// =============================================================================
// Bitmap Handles (LoadImageW / STM_SETIMAGE)
// =============================================================================

u32 CocoaBridge::register_bitmap_bmp(const std::vector<u8>& bmp_file) {
    if (bmp_file.size() < 14) {
        return 0;
    }

    // Parse dimensions from the DIB header (starts at offset 14)
    BitmapDims dims{};
    if (bmp_file.size() >= 14 + 12) {
        u32 hdr_size = static_cast<u32>(bmp_file[14]) |
                       (static_cast<u32>(bmp_file[15]) << 8) |
                       (static_cast<u32>(bmp_file[16]) << 16) |
                       (static_cast<u32>(bmp_file[17]) << 24);
        if (hdr_size == 12 && bmp_file.size() >= 14 + 12) {
            // BITMAPCOREHEADER: width(u16), height(u16), planes(u16), bpp(u16)
            dims.width  = static_cast<i32>(bmp_file[18] | (bmp_file[19] << 8));
            dims.height = static_cast<i32>(bmp_file[20] | (bmp_file[21] << 8));
            dims.planes = static_cast<u16>(bmp_file[22] | (bmp_file[23] << 8));
            dims.bpp    = static_cast<u16>(bmp_file[24] | (bmp_file[25] << 8));
        } else if (hdr_size >= 40 && bmp_file.size() >= 14 + 40) {
            // BITMAPINFOHEADER: width(i32), height(i32), planes(u16), bpp(u16)
            auto r32 = [&](size_t off) -> i32 {
                return static_cast<i32>(bmp_file[off] |
                       (bmp_file[off+1] << 8) |
                       (bmp_file[off+2] << 16) |
                       (bmp_file[off+3] << 24));
            };
            dims.width  = r32(14 + 4);
            dims.height = r32(14 + 8);
            dims.planes = static_cast<u16>(bmp_file[14+12] | (bmp_file[14+13] << 8));
            dims.bpp    = static_cast<u16>(bmp_file[14+14] | (bmp_file[14+15] << 8));
        }
    }

    __block u32 handle = 0;
    std::vector<u8> bytes_copy = bmp_file;

    vtr_run_on_main_sync(^{
        ensure_app_initialized();

        NSData* data = [NSData dataWithBytes:bytes_copy.data() length:bytes_copy.size()];
        NSImage* img = [[NSImage alloc] initWithData:data];
        if (!img) {
            VITREO_TRACE( "[cocoa] register_bitmap_bmp: NSImage decode failed (%zu bytes)\n",
                         bytes_copy.size());
            return;
        }

        handle = m_next_bitmap_handle++;
        m_bitmaps[handle] = (__bridge_retained void*)img;
        VITREO_TRACE( "[cocoa] register_bitmap_bmp: handle=0x%x size=%zu dims=%dx%d bpp=%u\n",
                     handle, bytes_copy.size(), dims.width, dims.height, dims.bpp);
    });

    if (handle != 0) {
        m_bitmap_dims[handle] = dims;
        m_bitmap_bmp_data[handle] = bmp_file;
    }

    return handle;
}

namespace {

/// One AppKit checkbox / radio glyph, scaled to fit @p frame and clipped to
/// it, in the current (flipped) graphics context.
void draw_button_glyph(NSRect frame, NSButtonType type, NSControlStateValue state, bool pushed,
                       bool enabled) {
    NSButtonCell* cell = [[NSButtonCell alloc] init];
    [cell setButtonType:type];
    [cell setTitle:@""];
    [cell setControlSize:frame.size.height >= 16 ? NSControlSizeRegular : NSControlSizeSmall];
    [cell setAllowsMixedState:YES];
    [cell setState:state];
    [cell setHighlighted:pushed];
    [cell setEnabled:enabled];
    // The glyph at its natural size, scaled to fit and clipped so a focus or
    // bezel inset cannot spill over.
    const NSSize natural = [cell cellSize];
    const CGFloat side = std::max<CGFloat>(1, std::max(natural.width, natural.height));
    const CGFloat scale = std::min(frame.size.width, frame.size.height) / side;
    [NSGraphicsContext saveGraphicsState];
    NSRectClip(frame);
    NSAffineTransform* place = [NSAffineTransform transform];
    [place translateXBy:frame.origin.x + (frame.size.width - side * scale) / 2
                    yBy:frame.origin.y + (frame.size.height - side * scale) / 2];
    [place scaleBy:scale];
    [place concat];
    [cell drawWithFrame:NSMakeRect((side - natural.width) / 2, (side - natural.height) / 2,
                                   natural.width, natural.height)
                 inView:nil];
    [NSGraphicsContext restoreGraphicsState];
    [cell release];
}

/// Runs @p draw on the main thread into a flipped, transparent @p width x
/// @p height RGBA context (light appearance: guest-painted surfaces are
/// light) and returns straight-alpha RGBA, top row first.
std::vector<u8> render_glyphs(int width, int height, void (^draw)(void)) {
    __block std::vector<u8> rgba(static_cast<size_t>(width) * height * 4, 0);
    vtr_run_on_main_sync(^{
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGContextRef cg = CGBitmapContextCreate(rgba.data(), width, height, 8, width * 4, space,
                                                kCGImageAlphaPremultipliedLast);
        CGColorSpaceRelease(space);
        if (!cg) return;
        CGContextTranslateCTM(cg, 0, height);
        CGContextScaleCTM(cg, 1, -1);
        NSGraphicsContext* gc = [NSGraphicsContext graphicsContextWithCGContext:cg flipped:YES];
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:gc];
        [[NSAppearance appearanceNamed:NSAppearanceNameAqua] performAsCurrentDrawingAppearance:draw];
        [NSGraphicsContext restoreGraphicsState];
        CGContextRelease(cg);
    });
    for (size_t i = 0; i < rgba.size(); i += 4) {  // premultiplied -> straight
        const u32 a = rgba[i + 3];
        if (a == 0 || a == 255) continue;
        for (int c = 0; c < 3; ++c) rgba[i + c] = static_cast<u8>(std::min<u32>(255, (rgba[i + c] * 255u + a / 2) / a));
    }
    return rgba;
}

}  // namespace

bool CocoaBridge::render_button_glyph(ButtonGlyph glyph, i32 width, i32 height,
                                      std::vector<u8>& rgba) {
    if (width <= 0 || height <= 0 || width > 512 || height > 512) return false;
    ensure_app_initialized();
    const NSButtonType type = glyph.radio ? NSButtonTypeRadio : NSButtonTypeSwitch;
    const NSControlStateValue state = glyph.mixed ? NSControlStateValueMixed
                                    : glyph.checked ? NSControlStateValueOn
                                                    : NSControlStateValueOff;
    const bool pushed = glyph.pushed, enabled = glyph.enabled;
    rgba = render_glyphs(width, height, ^{
        draw_button_glyph(NSMakeRect(0, 0, width, height), type, state, pushed, enabled);
    });
    return true;
}

u32 CocoaBridge::register_oem_bitmap(u32 id) {
    // OBM_CHECKBOXES: its shape is Windows' contract (52x39, 32 bpp; a 4x3 grid
    // of 13x13 cells, tools/oracle_probe/obm_bitmaps_probe.c): row 0 checkbox
    // {off, on, off pushed, on pushed}, row 1 radio button in the same four
    // states, row 2 {radio mask, empty, indeterminate checkbox, empty}. The
    // glyphs are AppKit's own checkbox and radio cells.
    constexpr u32 kObmCheckboxes = 32759;
    if (id != kObmCheckboxes) return 0;
    constexpr int kCell = 13, kWidth = 4 * kCell, kHeight = 3 * kCell;
    ensure_app_initialized();
    std::vector<u8> rgba = render_glyphs(kWidth, kHeight, ^{
        [[NSColor whiteColor] setFill];
        NSRectFill(NSMakeRect(0, 0, kWidth, kHeight));
        for (int row = 0; row < 2; ++row) {
            const NSButtonType type = row == 0 ? NSButtonTypeSwitch : NSButtonTypeRadio;
            for (int col = 0; col < 4; ++col) {
                draw_button_glyph(NSMakeRect(col * kCell, row * kCell, kCell, kCell), type,
                                  (col & 1) ? NSControlStateValueOn : NSControlStateValueOff,
                                  col >= 2, true);
            }
        }
        // Radio mask: white inside the radio's (round) shape, black outside.
        [[NSColor blackColor] setFill];
        NSRectFill(NSMakeRect(0, 2 * kCell, kCell, kCell));
        [[NSColor whiteColor] setFill];
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(0.5, 2 * kCell + 0.5, kCell - 1, kCell - 1)] fill];
        draw_button_glyph(NSMakeRect(2 * kCell, 2 * kCell, kCell, kCell), NSButtonTypeSwitch,
                          NSControlStateValueMixed, false, true);
    });
    // BMP file: BITMAPINFOHEADER, 32 bpp, bottom-up, opaque.
    const u32 stride = kWidth * 4, offset = 14 + 40;
    std::vector<u8> bmp(offset + stride * kHeight, 0);
    auto put16 = [&](u32 at, u32 v) { bmp[at] = v & 0xff; bmp[at + 1] = (v >> 8) & 0xff; };
    auto put32 = [&](u32 at, u32 v) { put16(at, v & 0xffff); put16(at + 2, v >> 16); };
    bmp[0] = 'B'; bmp[1] = 'M';
    put32(2, static_cast<u32>(bmp.size()));
    put32(10, offset);
    put32(14, 40);
    put32(18, kWidth);
    put32(22, kHeight);
    put16(26, 1);
    put16(28, 32);
    put32(34, stride * kHeight);
    for (int y = 0; y < kHeight; ++y) {
        const u8* src = rgba.data() + static_cast<size_t>(y) * stride;
        u8* dst = bmp.data() + offset + static_cast<size_t>(kHeight - 1 - y) * stride;
        for (int x = 0; x < kWidth; ++x) {
            dst[x * 4] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4];
            dst[x * 4 + 3] = 0xff;
        }
    }
    return register_bitmap_bmp(bmp);
}

bool CocoaBridge::get_bitmap_dims(u32 handle, BitmapDims& out) const {
    auto it = m_bitmap_dims.find(handle);
    if (it == m_bitmap_dims.end()) return false;
    out = it->second;
    return true;
}

const std::vector<u8>* CocoaBridge::get_bitmap_bmp_data(u32 handle) const {
    auto it = m_bitmap_bmp_data.find(handle);
    if (it == m_bitmap_bmp_data.end()) return nullptr;
    return &it->second;
}

bool CocoaBridge::set_control_bitmap(HWND ctrl_hwnd, u32 bitmap_handle) {
    if (![NSThread isMainThread]) {
        __block bool ok = false;
        vtr_run_on_main_sync(^{
            ok = this->set_control_bitmap(ctrl_hwnd, bitmap_handle);
        });
        return ok;
    }

    auto ctrl_it = m_controls.find(ctrl_hwnd);
    if (ctrl_it == m_controls.end()) {
        return false;
    }
    NSView* view = (__bridge NSView*)ctrl_it->second;
    if (![view isKindOfClass:[NSImageView class]]) {
        return false;
    }

    auto bmp_it = m_bitmaps.find(bitmap_handle);
    if (bmp_it == m_bitmaps.end() || bmp_it->second == nullptr) {
        NSImageView* iv = (NSImageView*)view;
        if ([iv image] != nil) {
            // Keep the currently displayed image to avoid flicker when apps
            // transiently send stale/unknown handles during startup.
            VITREO_TRACE( "[cocoa] set_control_bitmap: unknown handle=0x%x for hwnd=0x%x (preserve existing)\n",
                         bitmap_handle, ctrl_hwnd);
            return true;
        }
        VITREO_TRACE( "[cocoa] set_control_bitmap: unknown handle=0x%x for hwnd=0x%x\n",
                     bitmap_handle, ctrl_hwnd);
        return false;
    }

    NSImage* img = (__bridge NSImage*)bmp_it->second;
    [(NSImageView*)view setImage:img];
    [view setNeedsDisplay:YES];
    return true;
}

bool CocoaBridge::set_control_image_rgba(HWND ctrl_hwnd, u32 width, u32 height,
                                         const std::vector<u8>& rgba) {
    if (![NSThread isMainThread]) {
        __block bool ok = false;
        vtr_run_on_main_sync(^{
            ok = this->set_control_image_rgba(ctrl_hwnd, width, height, rgba);
        });
        return ok;
    }
    auto ctrl_it = m_controls.find(ctrl_hwnd);
    if (ctrl_it == m_controls.end()) return false;
    NSView* view = (__bridge NSView*)ctrl_it->second;
    if (![view isKindOfClass:[NSImageView class]]) return false;
    NSImageView* iv = (NSImageView*)view;
    if (rgba.empty() || width == 0 || height == 0 ||
        rgba.size() != static_cast<size_t>(width) * height * 4u) {
        [iv setImage:nil];
        return true;
    }
    NSData* copy = [NSData dataWithBytes:rgba.data() length:rgba.size()];
    CGDataProviderRef provider =
        CGDataProviderCreateWithCFData((__bridge CFDataRef)copy);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef cg = CGImageCreate(width, height, 8, 32, static_cast<size_t>(width) * 4u, cs,
                                  kCGImageAlphaLast | kCGBitmapByteOrderDefault, provider,
                                  nullptr, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(provider);
    if (cg == nullptr) return false;
    NSImage* img = [[NSImage alloc] initWithCGImage:cg size:NSMakeSize(width, height)];
    CGImageRelease(cg);
    [iv setImage:img];
    [view setNeedsDisplay:YES];
    return true;
}

bool CocoaBridge::set_control_image_data(HWND ctrl_hwnd, const u8* data, size_t len) {
    if (!data || len == 0) return false;

    if (![NSThread isMainThread]) {
        // Copy data for async dispatch
        std::vector<u8> copy(data, data + len);
        __block bool ok = false;
        vtr_run_on_main_sync(^{
            ok = this->set_control_image_data(ctrl_hwnd, copy.data(), copy.size());
        });
        return ok;
    }

    auto ctrl_it = m_controls.find(ctrl_hwnd);
    if (ctrl_it == m_controls.end()) {
        VITREO_TRACE( "[cocoa] set_control_image_data: control 0x%x not found\n", ctrl_hwnd);
        return false;
    }
    NSView* view = (__bridge NSView*)ctrl_it->second;
    if (![view isKindOfClass:[NSImageView class]]) {
        VITREO_TRACE( "[cocoa] set_control_image_data: control 0x%x is not NSImageView\n", ctrl_hwnd);
        return false;
    }

    NSData* nsdata = [NSData dataWithBytes:data length:len];
    NSImage* img = [[NSImage alloc] initWithData:nsdata];
    if (!img) {
        VITREO_TRACE( "[cocoa] set_control_image_data: NSImage decode failed (%zu bytes)\n", len);
        return false;
    }

    [(NSImageView*)view setImage:img];
    [view setNeedsDisplay:YES];
    VITREO_TRACE( "[cocoa] set_control_image_data: hwnd=0x%x set %zu byte image\n", ctrl_hwnd, len);
    return true;
}


void CocoaBridge::add_container_view(HWND parent_hwnd, HWND container_hwnd, const WindowData& data) {
    if (![NSThread isMainThread]) {
        WindowData data_copy = data;
        vtr_run_on_main_sync(^{
            this->add_container_view(parent_hwnd, container_hwnd, data_copy);
        });
        return;
    }

    NSView* content = nil;

    // Child dialogs parented to a tab control belong to successive tab pages.
    auto tab_it = m_tab_controls.find(parent_hwnd);
    if (tab_it != m_tab_controls.end() && tab_it->second.tab_view != nullptr) {
        content = (__bridge NSView*)tab_it->second.tab_view;
    }

    auto win_it = m_windows.find(parent_hwnd);
    if (!content && win_it != m_windows.end() && win_it->second.ns_window) {
        NSWindow* parent_win = reinterpret_cast<NSWindow*>(win_it->second.ns_window);
        content = [parent_win contentView];
    } else if (!content) {
        auto ctrl_it = m_controls.find(parent_hwnd);
        if (ctrl_it != m_controls.end()) {
            NSView* parent_view = (__bridge NSView*)ctrl_it->second;
            if ([parent_view isKindOfClass:[NSBox class]]) {
                content = [(NSBox*)parent_view contentView];
            } else {
                content = parent_view;
            }
        }
    }

    if (!content) {
        VITREO_TRACE( "[cocoa] add_container_view: no native parent for 0x%x\n",
                     parent_hwnd);
        return;
    }

    // Convert Windows coordinates to Cocoa, respecting flipped parents.
    CGFloat content_h = [content bounds].size.height;
    CGFloat x = static_cast<CGFloat>(data.rect.left);
    CGFloat w = static_cast<CGFloat>(data.rect.right - data.rect.left);
    CGFloat h = static_cast<CGFloat>(data.rect.bottom - data.rect.top);
    CGFloat y;
    if ([content isFlipped]) {
        y = static_cast<CGFloat>(data.rect.top);
    } else {
        y = content_h - static_cast<CGFloat>(data.rect.top) - h;
    }

    NSRect frame = NSMakeRect(x, y, w, h);
    NSView* container = [[VtrContainerView alloc] initWithFrame:frame];
    [container setAutoresizingMask:NSViewNotSizable];

    [content addSubview:container];

    if (!data.visible) {
        [container setHidden:YES];
    }

    // Dialog pages parented under a tab strip: track + show only the selected.
    if (tab_it != m_tab_controls.end() &&
        content == (__bridge NSView*)tab_it->second.tab_view) {
        const usize page_index = tab_it->second.next_page_index++;
        if (tab_it->second.page_views.size() <= page_index) {
            tab_it->second.page_views.resize(page_index + 1, nullptr);
        }
        tab_it->second.page_views[page_index] = (__bridge void*)container;
        const bool selected =
            static_cast<i32>(page_index) == tab_it->second.selected;
        [container setHidden:!selected || !data.visible];
    }

    m_controls[container_hwnd] = (__bridge void*)container;
    if ((data.ex_style & kWsExTransparentChild) != 0) {
        m_transparent_children.insert(container_hwnd);
    }
    // A new child can be the first opaque sibling above existing transparent
    // ones, so restack even when this child is not transparent itself.
    restack_transparent_children((__bridge void*)content);

    VITREO_TRACE( "[cocoa] add_container_view: parent=0x%x child=0x%x frame=(%.0f,%.0f,%.0f,%.0f) visible=%s\n",
                 parent_hwnd, container_hwnd,
                 frame.origin.x, frame.origin.y, frame.size.width, frame.size.height,
                 data.visible ? "yes" : "no");
}

} // namespace vitreo::windowing

namespace vitreo::windowing {

std::optional<std::pair<i32, i32>> CocoaBridge::native_control_image_size(HWND hwnd) const {
    __block std::optional<std::pair<i32, i32>> out;
    vtr_run_on_main_sync(^{
        auto it = m_controls.find(hwnd);
        if (it == m_controls.end()) return;
        NSView* view = (__bridge NSView*)it->second;
        if (![view isKindOfClass:[NSImageView class]]) return;
        NSImage* img = [(NSImageView*)view image];
        if (img == nil) return;
        out = std::make_pair(static_cast<i32>(img.size.width), static_cast<i32>(img.size.height));
    });
    return out;
}

}  // namespace vitreo::windowing
