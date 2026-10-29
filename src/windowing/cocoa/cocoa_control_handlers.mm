// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_control_handlers.mm
/// @brief The target and delegate objects behind standard controls.
///
/// AppKit reports a button click, an edit change, a menu choice or a tree
/// selection to an object; Win32 reports it to a window. The handlers here
/// turn each callback into the WM_COMMAND or WM_NOTIFY the guest expects,
/// and the CocoaBridge helpers at the end queue those notifications.
///
/// Split out of cocoa_bridge.mm, which was 7,624 lines. The code is
/// unchanged; only its translation unit is new.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CATransaction.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAShapeLayer.h>
#import <objc/runtime.h>
#include "windowing/model/message_box_layout.hpp"
#include "windowing/model/diag_flags.hpp"
#include "windowing/cocoa/objc_helpers.h"
#include "windowing/cocoa/cocoa_control_delegates.h"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/messages/input_latency.hpp"
#include "ipc/hwnd_registry.hpp"
#include "windowing/cocoa/window_presentation.hpp"
#include "windowing/cocoa/app_icon_presentation.hpp"
#include "graphics/common/perf_diag.hpp"
#include "graphics/common/surface_presenter.hpp"
#include <string>
#include <cstring>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cmath>
#include <limits>
#include <algorithm>
#include <CoreFoundation/CFRunLoop.h>
#include <Block.h>
#include <deque>
#include <mutex>
#include <atomic>
#include <CoreGraphics/CGEventSource.h>
#include <libproc.h>
#include <cerrno>
#include <signal.h>
#include <mach/mach_time.h>
#include "windowing/cocoa/cocoa_bridge_internal.h"

// =============================================================================
// VtrButtonHandler — posts WM_COMMAND when button is clicked
// =============================================================================

@implementation VtrButtonHandler

- (void)buttonClicked:(id)sender {
    (void)sender;
    if (diag_logs_enabled()) {
        std::fprintf(stderr, "[btn-diag] buttonClicked: parent=0x%x ctrl=0x%x id=%u queue=%p\n",
                    _parentHwnd, _controlHwnd, _controlId, (void*)_queue);
    }
    // MS Learn BS_AUTORADIOBUTTON / BS_AUTOCHECKBOX: the system updates check
    // state before BN_CLICKED. Sync Cocoa (+ sibling radios) so BM_GETCHECK in
    // the parent dlgproc sees the new state (a wizard's license-accept button).
    if (_bridge) {
        _bridge->apply_auto_button_click(_controlHwnd);
    }
    if (_queue) {
        // WM_COMMAND: HIWORD(wParam) = notification (BN_CLICKED = 0)
        //             LOWORD(wParam) = control ID
        //             lParam = control HWND
        vitreo::windowing::MSG msg{};
        msg.hwnd    = _parentHwnd;
        msg.message = vitreo::windowing::WM::COMMAND;
        // MAKEWPARAM(LOWORD(id), BN_CLICKED): the ID is truncated to 16 bits.
        // VCL sets GWL_ID = HWND (e.g. 0x10050); unmasked, bit 16 read as
        // BN_DOUBLECLICKED and the button never clicked.
        msg.wParam  = static_cast<vitreo::u64>(_controlId & 0xFFFFu);
        msg.lParam  = static_cast<vitreo::u64>(_controlHwnd);
        _queue->post(msg);

        VITREO_TRACE( "[cocoa] Button clicked: parent=0x%x id=%u\n",
                     _parentHwnd, _controlId);
    }
}

// Per-Win32-radio-group click actions. macOS groups NSButtonTypeRadio buttons by
// (action, superview); radios in the same superview that share a selector form
// ONE native group. Win32 dialogs delimit radio groups with
// WS_GROUP, so we give each group a distinct selector here — macOS then treats
// each WS_GROUP group as an independent radio group and auto-manages its single
// selection, while every selector still forwards to the common WM_COMMAND post.
- (void)radioClick0:(id)s  { [self buttonClicked:s]; }
- (void)radioClick1:(id)s  { [self buttonClicked:s]; }
- (void)radioClick2:(id)s  { [self buttonClicked:s]; }
- (void)radioClick3:(id)s  { [self buttonClicked:s]; }
- (void)radioClick4:(id)s  { [self buttonClicked:s]; }
- (void)radioClick5:(id)s  { [self buttonClicked:s]; }
- (void)radioClick6:(id)s  { [self buttonClicked:s]; }
- (void)radioClick7:(id)s  { [self buttonClicked:s]; }
- (void)radioClick8:(id)s  { [self buttonClicked:s]; }
- (void)radioClick9:(id)s  { [self buttonClicked:s]; }
- (void)radioClick10:(id)s { [self buttonClicked:s]; }
- (void)radioClick11:(id)s { [self buttonClicked:s]; }
- (void)radioClick12:(id)s { [self buttonClicked:s]; }
- (void)radioClick13:(id)s { [self buttonClicked:s]; }
- (void)radioClick14:(id)s { [self buttonClicked:s]; }
- (void)radioClick15:(id)s { [self buttonClicked:s]; }

@end


@implementation VtrEditHandler
- (void)controlTextDidChange:(NSNotification *)note {
    (void)note;
    if (_bridge) {
        // WM_COMMAND: HIWORD(wParam)=EN_CHANGE(0x300), LOWORD=ctrl id; lParam=hwnd.
        _bridge->post_window_message(_parentHwnd, vitreo::windowing::WM::COMMAND,
            (static_cast<vitreo::u64>([[note object] isKindOfClass:[NSComboBox class]] ? 5u : 0x0300u) << 16) | (_controlId & 0xFFFFu),
            static_cast<vitreo::u64>(_controlHwnd));
    }
}
- (void)comboBoxSelectionDidChange:(NSNotification*)note {
    (void)note;
    if (_bridge) _bridge->post_window_message(_parentHwnd,
        vitreo::windowing::WM::COMMAND,
        (static_cast<vitreo::u64>(1u) << 16) | (_controlId & 0xffffu),
        _controlHwnd); // CBN_SELCHANGE
}
@end


@implementation VtrRichEditScrollHandler
- (void)boundsDidChange:(NSNotification*)note {
    (void)note;
    if (!_bridge) return;
    // Prefer the live Win32 control ID from WindowData — CreateWindowEx stores
    // it in ctrl_id, while dialog templates also stash it in user_data.
    if (_controlId == 0) {
        // Fall back stays 0; parent still gets WM_VSCROLL with lParam=hwnd.
    }
    _bridge->notify_richedit_scroll(_controlHwnd);
}
@end

// =============================================================================
// VtrMenuItemHandler — posts WM_COMMAND when a native menu item is clicked
// =============================================================================

@implementation VtrMenuItemHandler

- (void)menuItemClicked:(id)sender {
    if ([sender isKindOfClass:[NSMenuItem class]] &&
        vtr_native_text_editor_focused([NSApp keyWindow])) {
        NSMenuItem* item = (NSMenuItem*)sender;
        if (item.keyEquivalentModifierMask & NSEventModifierFlagCommand) {
            SEL action = nullptr;
            NSString* key = item.keyEquivalent;
            if ([key isEqualToString:@"x"]) action = @selector(cut:);
            else if ([key isEqualToString:@"c"]) action = @selector(copy:);
            else if ([key isEqualToString:@"v"]) action = @selector(paste:);
            else if ([key isEqualToString:@"a"]) action = @selector(selectAll:);
            else if ([key isEqualToString:@"z"]) action = (item.keyEquivalentModifierMask & NSEventModifierFlagShift) ? @selector(redo:) : @selector(undo:);
            if (action && [NSApp sendAction:action to:nil from:sender]) return;
        }
    }
    if (_queue) {
        vitreo::windowing::MSG msg{};
        msg.hwnd    = _targetHwnd;
        msg.message = vitreo::windowing::WM::COMMAND;
        // WM_COMMAND from menu: HIWORD(wParam) = 0, LOWORD(wParam) = menu item ID
        msg.wParam  = _menuItemId;
        msg.lParam  = 0;
        _queue->post(msg);

        VITREO_TRACE( "[cocoa] Menu item clicked: hwnd=0x%x id=%u\n",
                     _targetHwnd, _menuItemId);
    }
}

@end

// Global array of menu item handlers — must be declared before pump_events
// which updates their queue pointers.
static NSMutableArray* s_menu_handlers = nil;

NSMutableArray* vtr_menu_handlers() { return s_menu_handlers; }
void vtr_set_menu_handlers(NSMutableArray* handlers) { s_menu_handlers = handlers; }

// =============================================================================
// SysTreeView32 — NSOutlineView-backed tree control (global scope: ObjC
// declarations may not appear inside a C++ namespace).
// =============================================================================

// This file is compiled without ARC: an ivar assigned in -init must own its
// object (+1 from alloc), or it dangles once the autorelease pool drains and
// the next message lands on whatever reused the memory (a settings tree
// died in treeview_insert_item with an unrecognized selector).
@implementation VtrTreeNode
- (instancetype)init {
    if (self = [super init]) {
        _children = [[NSMutableArray alloc] init];
    }
    return self;
}
- (void)dealloc {
    [_children release];
    [_text release];
    [super dealloc];
}
@end


@implementation VtrListBoxController
- (NSInteger)numberOfRowsInTableView:(NSTableView*)tv {
    (void)tv;
    return _bridge ? (NSInteger)_bridge->listbox_get_count(_hwnd) : 0;
}
- (id)tableView:(NSTableView*)tv objectValueForTableColumn:(NSTableColumn*)col row:(NSInteger)row {
    (void)tv; (void)col;
    if (!_bridge) return @"";
    std::wstring s = _bridge->listbox_get_item_text(_hwnd, (vitreo::i32)row);
    NSString* ns = [[NSString alloc] initWithBytes:s.data()
                                            length:s.size() * sizeof(wchar_t)
                                          encoding:NSUTF32LittleEndianStringEncoding];
    return ns ?: @"";
}
- (void)tableViewSelectionDidChange:(NSNotification*)note {
    if (_suppressNotify) return;
    NSTableView* tv = (NSTableView*)note.object;
    NSInteger row = [tv selectedRow];
    if (_bridge) _bridge->set_listbox_ui_selection(_hwnd, (vitreo::i32)row);
    if (_queue && _parent) {
        // WM_COMMAND: HIWORD(wParam)=LBN_SELCHANGE(1), LOWORD=ctrl id; lParam=hwnd
        vitreo::windowing::MSG msg{};
        msg.hwnd    = _parent;
        msg.message = vitreo::windowing::WM::COMMAND;
        msg.wParam  = ((vitreo::u64)1u << 16) | (_ctrlId & 0xFFFFu);
        msg.lParam  = (vitreo::u64)_hwnd;
        _queue->post(msg);
    }
}
- (void)doubleClicked:(id)sender {
    NSTableView* tv = (NSTableView*)sender;
    if (_bridge) _bridge->set_listbox_ui_selection(_hwnd, (vitreo::i32)[tv clickedRow]);
    if (_queue && _parent) {
        // LBN_DBLCLK = 2 (a double-click on a list row is the Load verb)
        vitreo::windowing::MSG msg{};
        msg.hwnd    = _parent;
        msg.message = vitreo::windowing::WM::COMMAND;
        msg.wParam  = ((vitreo::u64)2u << 16) | (_ctrlId & 0xFFFFu);
        msg.lParam  = (vitreo::u64)_hwnd;
        _queue->post(msg);
    }
}
@end

@implementation VtrTabStripView
- (BOOL)isFlipped { return YES; }

- (void)reloadTabs {
    [self setNeedsDisplay:YES];
}

- (CGFloat)stripHeight {
    return 26.0;
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    if (_bridge && _hwnd) {
        _bridge->draw_tab_strip(_hwnd);
    }
}

- (NSView*)hitTest:(NSPoint)point {
    const NSPoint local = [self convertPoint:point fromView:[self superview]];
    if (local.y > [self stripHeight]) {
        // Display area: Win32 DocTab hosts Scintilla as a sibling underneath.
        return nil;
    }
    return [super hitTest:point];
}

- (void)mouseDown:(NSEvent*)event {
    if (!_bridge || !_hwnd) return;
    const NSPoint local = [self convertPoint:[event locationInWindow]
                                    fromView:nil];
    if (local.y > [self stripHeight]) return;
    // Resolve which tab cell was hit using the same metrics as TCM_GETITEMRECT.
    vitreo::windowing::Rect item{};
    for (vitreo::i32 i = 0; ; ++i) {
        if (!_bridge->tab_get_item_rect(_hwnd, i, item)) break;
        const NSRect r = NSMakeRect(item.left, item.top,
                                    item.right - item.left,
                                    item.bottom - item.top);
        if (NSPointInRect(local, r)) {
            _bridge->tab_strip_clicked(_hwnd, i, !_suppressNotify);
            return;
        }
    }
}
@end

@implementation VtrTabController
@end

@implementation VtrTreeController
- (instancetype)init {
    if (self = [super init]) {
        _roots = [[NSMutableArray alloc] init];
        _nodeByItem = [[NSMutableDictionary alloc] init];
    }
    return self;
}
- (void)dealloc {
    [_roots release];
    [_nodeByItem release];
    [super dealloc];
}
- (NSInteger)outlineView:(NSOutlineView*)ov numberOfChildrenOfItem:(id)item {
    (void)ov;
    if (item == nil) return (NSInteger)_roots.count;
    return (NSInteger)((VtrTreeNode*)item).children.count;
}
- (id)outlineView:(NSOutlineView*)ov child:(NSInteger)index ofItem:(id)item {
    (void)ov;
    NSArray* arr = (item == nil) ? _roots : ((VtrTreeNode*)item).children;
    if ((NSUInteger)index >= arr.count) return nil;
    return arr[(NSUInteger)index];
}
- (BOOL)outlineView:(NSOutlineView*)ov isItemExpandable:(id)item {
    (void)ov;
    VtrTreeNode* node = (VtrTreeNode*)item;
    return node.hasChildren || node.children.count > 0;
}
- (NSView*)outlineView:(NSOutlineView*)ov viewForTableColumn:(NSTableColumn*)col item:(id)item {
    (void)col;
    if (!item) return nil;
    VtrTreeNode* node = (VtrTreeNode*)item;
    NSTextField* tf = [ov makeViewWithIdentifier:@"VtrTreeCell" owner:self];
    if (!tf) {
        tf = [[NSTextField alloc] init];
        tf.identifier = @"VtrTreeCell";
        [tf setBordered:NO];
        [tf setEditable:NO];
        [tf setSelectable:NO];
        [tf setDrawsBackground:NO];
        [tf setFont:[NSFont systemFontOfSize:[NSFont systemFontSize]]];
        [tf setLineBreakMode:NSLineBreakByTruncatingTail];
    }
    [tf setStringValue:node.text ?: @""];
    return tf;
}
- (void)outlineViewSelectionDidChange:(NSNotification*)note {
    if (vitreo::windowing::diag::win_diag())
        fprintf(stderr, "[nav] selectionDidChange suppress=%d\n", _suppressNotify ? 1 : 0);
    if (_suppressNotify) return;
    NSOutlineView* ov = note.object;
    NSInteger row = [ov selectedRow];
    if (row < 0) return;
    VtrTreeNode* node = [ov itemAtRow:row];
    if (!node || !_bridge) return;
    if (vitreo::windowing::diag::win_diag())
        fprintf(stderr, "[nav] click row=%ld hItem=%llu\n", (long)row, (unsigned long long)node.hItem);
    // Record selection + enqueue a WM_NOTIFY(TVN_SELCHANGED) for the loop.
    _bridge->enqueue_tree_notify(_hwnd, node.hItem);
}
- (void)outlineViewItemWillExpand:(NSNotification*)note {
    if (_suppressNotify) return;
    VtrTreeNode* node = note.userInfo[@"NSObject"];
    if (node && _bridge) _bridge->enqueue_tree_expand(_hwnd, node.hItem);
}
- (void)outlineViewItemWillCollapse:(NSNotification*)note {
    if (_suppressNotify) return;
    VtrTreeNode* node = note.userInfo[@"NSObject"];
    if (node && _bridge) _bridge->enqueue_tree_collapse(_hwnd, node.hItem);
}
@end

namespace vitreo::windowing {

// Internal helper used by the VtrTreeController (global scope, above) to record
// + queue a user-click selection. The message loop drains m_tree_notifies and
// synthesizes WM_NOTIFY(TVN_SELCHANGED).
void CocoaBridge::post_window_message(HWND hwnd, u32 msg, u64 wParam, u64 lParam) {
    if (!m_queue) return;
    MSG m{};
    m.hwnd    = hwnd;
    m.message = msg;
    m.wParam  = wParam;
    m.lParam  = lParam;
    m_queue->post(m);
}

void CocoaBridge::enqueue_tree_notify(HWND ctrl_hwnd, u64 h_item) {
    auto it = m_treeviews.find(ctrl_hwnd);
    if (it == m_treeviews.end()) return;
    it->second.selected = h_item;
    TreeNotify n{};
    n.parent  = it->second.parent;
    n.ctrl    = ctrl_hwnd;
    n.ctrl_id = it->second.ctrl_id;
    n.h_item  = h_item;
    auto iit = it->second.items.find(h_item);
    n.lparam  = (iit != it->second.items.end()) ? iit->second.lparam : 0;
    {
        std::lock_guard<std::mutex> lk(m_tree_notify_mutex);
        m_tree_notifies.push_back(n);
    }
    m_has_tree_notify.store(true, std::memory_order_release);
    // GetMessage synthesizes the WM_NOTIFY; wake it rather than wait for a poll.
    if (m_queue != nullptr) m_queue->notify_all_threads();
    if (diag::win_diag())
        fprintf(stderr, "[nav] enqueue parent=0x%x ctrl_id=%u hItem=%llu lparam=0x%llx\n",
                n.parent, n.ctrl_id, (unsigned long long)n.h_item,
                (unsigned long long)n.lparam);
}

void CocoaBridge::enqueue_tree_expand(HWND ctrl_hwnd, u64 h_item) {
    auto it = m_treeviews.find(ctrl_hwnd);
    if (it == m_treeviews.end()) return;
    TreeNotify notify{};
    notify.parent = it->second.parent;
    notify.ctrl = ctrl_hwnd;
    notify.ctrl_id = it->second.ctrl_id;
    notify.h_item = h_item;
    notify.kind = TreeNotify::Kind::Expanding;
    auto item = it->second.items.find(h_item);
    notify.lparam = item != it->second.items.end() ? item->second.lparam : 0;
    {
        std::lock_guard<std::mutex> lock(m_tree_notify_mutex);
        m_tree_notifies.push_back(notify);
    }
    m_has_tree_notify.store(true, std::memory_order_release);
    // GetMessage synthesizes the WM_NOTIFY; wake it rather than wait for a poll.
    if (m_queue != nullptr) m_queue->notify_all_threads();
}

void CocoaBridge::enqueue_tree_collapse(HWND ctrl_hwnd, u64 h_item) {
    auto it = m_treeviews.find(ctrl_hwnd);
    if (it == m_treeviews.end()) return;
    TreeNotify notify{};
    notify.parent = it->second.parent;
    notify.ctrl = ctrl_hwnd;
    notify.ctrl_id = it->second.ctrl_id;
    notify.h_item = h_item;
    notify.kind = TreeNotify::Kind::Collapsing;
    auto item = it->second.items.find(h_item);
    notify.lparam = item != it->second.items.end() ? item->second.lparam : 0;
    {
        std::lock_guard<std::mutex> lock(m_tree_notify_mutex);
        m_tree_notifies.push_back(notify);
    }
    m_has_tree_notify.store(true, std::memory_order_release);
    // GetMessage synthesizes the WM_NOTIFY; wake it rather than wait for a poll.
    if (m_queue != nullptr) m_queue->notify_all_threads();
}

} // namespace vitreo::windowing
