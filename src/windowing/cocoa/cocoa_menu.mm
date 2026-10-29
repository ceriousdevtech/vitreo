// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_menu.mm
/// @brief The native macOS menu bar, driven by Win32 menu APIs.
///
/// Win32 gives each top-level window its own menu, owned by that window;
/// macOS has one menu bar owned by the application and swapped as focus
/// moves. Every function here bridges that difference — a guest calling
/// AppendMenu or CheckMenuItem addresses its own window's menu, and the
/// result has to land on the single shared bar.
///
/// Split out of cocoa_bridge.mm. All methods here are declared in
/// cocoa_bridge.hpp, so this file adds no interface of its own.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CATransaction.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAShapeLayer.h>
#import <objc/runtime.h>
#include "windowing/cocoa/objc_helpers.h"
#include "windowing/cocoa/cocoa_perf_overlay.h"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/cocoa/app_icon_presentation.hpp"
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

#include "windowing/cocoa/objc_helpers.h"
#include "windowing/cocoa/cocoa_bridge.hpp"

void vtr_ensure_enter_full_screen_menu(NSMenu* mainMenu) {
    if (!mainMenu) return;
    NSMenu* viewMenu = nil;
    for (NSMenuItem* item in [mainMenu itemArray]) {
        if ([[item title] isEqualToString:@"View"] && [item hasSubmenu]) {
            viewMenu = [item submenu];
            break;
        }
    }
    if (!viewMenu) {
        NSMenuItem* viewItem = [[NSMenuItem alloc] initWithTitle:@"View"
                                                          action:nil
                                                   keyEquivalent:@""];
        viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
        [viewItem setSubmenu:viewMenu];
        [mainMenu addItem:viewItem];
    }
    vtr_perf_overlay_add_menu_item(viewMenu);
    for (NSMenuItem* item in [viewMenu itemArray]) {
        if ([item action] == @selector(toggleFullScreen:)) return;
    }
    if ([viewMenu numberOfItems] > 0) {
        [viewMenu addItem:[NSMenuItem separatorItem]];
    }
    NSMenuItem* fs = [[NSMenuItem alloc] initWithTitle:@"Enter Full Screen"
                                                action:@selector(toggleFullScreen:)
                                         keyEquivalent:@"f"];
    [fs setKeyEquivalentModifierMask:NSEventModifierFlagControl |
                                     NSEventModifierFlagCommand];
    [viewMenu addItem:fs];
}

namespace vitreo::windowing {

// =============================================================================
// Native macOS Menu Bar
// =============================================================================

// The handler array is shared with the event pump, which refreshes each
// handler's queue pointer; see vtr_menu_handlers() in objc_helpers.h.

void CocoaBridge::set_app_name(const std::string& name) {
    vtr_run_on_main_sync(^{
        NSString* appName = [NSString stringWithUTF8String:name.c_str()];
        if (!appName || [appName length] == 0) return;

        // Do this before sharedApplication so AppKit begins with the guest
        // process and application-menu name. LaunchServices independently
        // gets the Dock label from the generated app bundle.
        set_process_application_name(appName);
        ensure_app_initialized();

        // Update the main menu's app menu title
        NSMenu* mainMenu = [NSApp mainMenu];
        if (mainMenu && [[mainMenu itemArray] count] > 0) {
            NSMenuItem* appMenuItem = [[mainMenu itemArray] firstObject];
            [appMenuItem setTitle:appName];
            if ([appMenuItem hasSubmenu]) {
                // Update the "About <name>" and "Quit <name>" items
                NSMenu* appMenu = [appMenuItem submenu];
                [appMenu setTitle:appName];
                for (NSMenuItem* item in [appMenu itemArray]) {
                    NSString* title = [item title];
                    if ([title hasPrefix:@"About "]) {
                        [item setTitle:[NSString stringWithFormat:@"About %@", appName]];
                    } else if ([title hasPrefix:@"Quit "]) {
                        [item setTitle:[NSString stringWithFormat:@"Quit %@", appName]];
                    }
                }
            }
        }

        VITREO_TRACE( "[cocoa] set_app_name: identity='%s' initialized=%s\n",
                     name.c_str(), m_app_initialized ? "yes" : "no");
    });
}

void CocoaBridge::set_app_icon(const std::vector<u8>& ico_data) {
    if (ico_data.empty()) return;

    if (main_bundle_declares_application_icon()) {
        VITREO_TRACE( "[cocoa] set_app_icon: bundle icon is authoritative; "
                     "skipping dynamic override\n");
        m_pending_icon.clear();
        return;
    }

    if (m_app_launched) {
        // App already launched — apply immediately
        std::vector<u8> data_copy = ico_data;
        vtr_run_on_main_sync(^{
            NSData* ns_data = [NSData dataWithBytes:data_copy.data()
                                             length:data_copy.size()];
            NSImage* source_icon =
                [[[NSImage alloc] initWithData:ns_data] autorelease];
            NSImage* icon = create_macos_presented_app_icon(source_icon);
            if (!icon) icon = source_icon;
            if (icon) {
                [NSApp setApplicationIconImage:icon];
                VITREO_TRACE(
                    "[cocoa] set_app_icon: applied native-presented fallback "
                    "immediately (%zu bytes)\n",
                    data_copy.size());
            }
        });
    } else {
        // Defer until finish_app_launch when the dock tile exists
        m_pending_icon = ico_data;
        VITREO_TRACE( "[cocoa] set_app_icon: deferred %zu bytes until app launch\n",
                     ico_data.size());
    }
}

void CocoaBridge::install_menu_bar(HWND hwnd, const Win32MenuBar& menu_bar) {
    // Save the target HWND for future AppendMenuW calls
    m_menu_target_hwnd = hwnd;

    // Merge any queued checks (from calls before first install) into pending
    // so the block can pick them up. The actual NSMenuItem state snapshot is
    // done inside the main-thread block below.
    auto pending_copy = m_pending_check_states;
    m_pending_check_states.clear();

    // We need to capture the data for the block — copy the menu structure
    Win32MenuBar menu_copy = menu_bar;
    HWND target_hwnd = hwnd;
    MessageQueue* queue = m_queue;

    vtr_run_on_main_sync(^{
        ensure_app_initialized();

        if (!vtr_menu_handlers()) {
            vtr_set_menu_handlers([[NSMutableArray alloc] init]);
        }

        // Snapshot check states from existing NSMenuItems (on main thread)
        std::unordered_map<u16, bool> saved_checks = pending_copy;
        for (auto& [id, ptr] : m_menu_items_by_id) {
            if (ptr) {
                NSMenuItem* item = (__bridge NSMenuItem*)ptr;
                if ([item state] == NSControlStateValueOn) {
                    saved_checks[id] = true;
                }
            }
        }

        NSMenu* mainMenu = [[NSMenu alloc] initWithTitle:@"MainMenu"];

        // Derive app name from the window title
        NSString* appName = @"";
        auto win_it = m_windows.find(target_hwnd);
        if (win_it != m_windows.end() && !win_it->second.title.empty()) {
            appName = to_nsstring(win_it->second.title);
            VITREO_TRACE( "[cocoa] install_menu_bar: using title '%ls' for app name\n",
                         win_it->second.title.c_str());
        } else {
            VITREO_TRACE( "[cocoa] install_menu_bar: hwnd=0x%x not found in m_windows (size=%zu)\n",
                         target_hwnd, m_windows.size());
        }

        // Add the application menu (first item — Quit, etc.)
        {
            NSMenuItem* appMenuItem = [[NSMenuItem alloc] initWithTitle:appName
                                                                action:nil
                                                         keyEquivalent:@""];
            NSMenu* appMenu = [[NSMenu alloc] initWithTitle:appName];
            // "Quit" with Cmd+Q
            NSString* quitTitle = [appName length] > 0
                ? [NSString stringWithFormat:@"Quit %@", appName]
                : @"Quit";
            NSMenuItem* quitItem = [[NSMenuItem alloc]
                initWithTitle:quitTitle
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
            [appMenu addItem:quitItem];
            [appMenuItem setSubmenu:appMenu];
            [mainMenu addItem:appMenuItem];
        }

        // A guest-supplied menu owns its command groups. Do not infer missing
        // groups from display labels: shortcuts can live only in ACCEL tables.
        if (menu_copy.items.empty()) {
            NSMenuItem* editItem = [[NSMenuItem alloc] initWithTitle:@"Edit"
                                                              action:nil
                                                       keyEquivalent:@""];
            NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
            [editMenu addItemWithTitle:@"Cut"
                                action:@selector(cut:)
                         keyEquivalent:@"x"];
            [editMenu addItemWithTitle:@"Copy"
                                action:@selector(copy:)
                         keyEquivalent:@"c"];
            [editMenu addItemWithTitle:@"Paste"
                                action:@selector(paste:)
                         keyEquivalent:@"v"];
            [editMenu addItem:[NSMenuItem separatorItem]];
            [editMenu addItemWithTitle:@"Select All"
                                action:@selector(selectAll:)
                         keyEquivalent:@"a"];
            [editItem setSubmenu:editMenu];
            [mainMenu addItem:editItem];
        }

        // Preserve the complete guest menu hierarchy, including nested groups.
        auto populate = [&](auto&& self, NSMenu* native, const std::vector<Win32MenuItem>& items) -> void {
            [native setAutoenablesItems:NO];
            for (const auto& child : items) {
                if (child.is_separator) { [native addItem:[NSMenuItem separatorItem]]; continue; }
                NSString* label = strip_win_accelerator(to_nsstring(child.text));
                NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:label action:nil keyEquivalent:@""];
                if (child.is_popup) {
                    NSMenu* nested = [[NSMenu alloc] initWithTitle:label];
                    self(self, nested, child.children);
                    [item setSubmenu:nested];
                } else {
                    // An ampersand is a Windows mnemonic, not a Command shortcut.
                    // Translate explicit shortcut hints, preserving modifiers.
                    NSString* key = @"";
                    NSEventModifierFlags modifiers = 0;
                    NSRange tab = [label rangeOfString:@"\t"];
                    if (tab.location != NSNotFound) {
                        NSString* hint = [[label substringFromIndex:tab.location + 1] lowercaseString];
                        NSArray<NSString*>* pieces = [hint componentsSeparatedByString:@"+"];
                        NSString* last = [[pieces lastObject] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
                        if ([pieces containsObject:@"ctrl"]) modifiers |= NSEventModifierFlagCommand;
                        if ([pieces containsObject:@"shift"]) modifiers |= NSEventModifierFlagShift;
                        if ([pieces containsObject:@"alt"]) modifiers |= NSEventModifierFlagOption;
                        if (last.length == 1) key = last;
                        else if ([last hasPrefix:@"f"] && last.length <= 3) {
                            const NSInteger n = [[last substringFromIndex:1] integerValue];
                            if (n >= 1 && n <= 24) key = [NSString stringWithFormat:@"%C", static_cast<unichar>(NSF1FunctionKey + n - 1)];
                        }
                        label = [label substringToIndex:tab.location];
                    }
                    VtrMenuItemHandler* handler = [[VtrMenuItemHandler alloc] init];
                    handler.targetHwnd = target_hwnd;
                    handler.menuItemId = child.id;
                    handler.queue = queue;
                    [item setTitle:label];
                    [item setAction:@selector(menuItemClicked:)];
                    [item setTarget:handler];
                    [item setTag:child.id];
                    [item setKeyEquivalent:key];
                    [item setKeyEquivalentModifierMask:modifiers];
                    static char handlerKey;
                    objc_setAssociatedObject(item, &handlerKey, handler, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
                    m_menu_items_by_id[child.id] = (__bridge_retained void*)item;
                }
                [item setState:child.is_checked ? NSControlStateValueOn : NSControlStateValueOff];
                [item setEnabled:!child.is_grayed];
                [native addItem:item];
            }
        };
        populate(populate, mainMenu, menu_copy.items);

        vtr_ensure_enter_full_screen_menu(mainMenu);

        [NSApp setMainMenu:mainMenu];

        // Update the process name AFTER installing the menu, so macOS picks it up
        // for the menu bar bold text. For non-bundled executables, macOS uses
        // NSProcessInfo.processName for the app menu bold label.
        if ([appName length] > 0) {
            [[NSProcessInfo processInfo] setValue:appName forKey:@"processName"];
        }

        VITREO_TRACE( "[cocoa] install_menu_bar: hwnd=0x%x, %zu top-level menus\n",
                     target_hwnd, menu_copy.items.size());

        // Restore saved check states (from previous menu build or queued calls)
        for (auto& [id, chk] : saved_checks) {
            if (!chk) continue;  // only need to apply ON states
            auto it2 = m_menu_items_by_id.find(id);
            if (it2 != m_menu_items_by_id.end() && it2->second) {
                NSMenuItem* mi = (__bridge NSMenuItem*)it2->second;
                [mi setState:NSControlStateValueOn];
            }
        }
    });
}

void CocoaBridge::check_menu_item(u16 item_id, bool checked) {
    // If the menu bar hasn't been installed yet, queue the state for later.
    if (m_menu_items_by_id.empty()) {
        m_pending_check_states[item_id] = checked;
        return;
    }
    vtr_run_on_main_sync(^{
        auto it = m_menu_items_by_id.find(item_id);
        if (it != m_menu_items_by_id.end() && it->second) {
            NSMenuItem* item = (__bridge NSMenuItem*)it->second;
            [item setState:checked ? NSControlStateValueOn : NSControlStateValueOff];
        }
    });
}

void CocoaBridge::enable_menu_item(u16 item_id, bool enabled) {
    vtr_run_on_main_sync(^{
        auto it = m_menu_items_by_id.find(item_id);
        if (it != m_menu_items_by_id.end() && it->second) {
            NSMenuItem* item = (__bridge NSMenuItem*)it->second;
            [item setEnabled:enabled];
            // Also need to disable auto-enable so macOS respects our state
            [[item menu] setAutoenablesItems:NO];
        }
    });
}

void CocoaBridge::register_popup_menu(u32 hmenu) {
    vtr_run_on_main_sync(^{
        NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
        [menu setAutoenablesItems:NO];
        m_submenus_by_handle[hmenu] = (__bridge_retained void*)menu;
        VITREO_TRACE( "[cocoa] register_popup_menu: hmenu=0x%x\n", hmenu);
    });
}

void CocoaBridge::append_menu_item(u32 hmenu, u16 item_id,
                                   const std::string& text, bool is_separator) {
    std::string text_copy = text;
    HWND target = m_menu_target_hwnd;
    MessageQueue* queue = m_queue;

    vtr_run_on_main_sync(^{
        // Find the target NSMenu
        NSMenu* targetMenu = nil;
        auto it = m_submenus_by_handle.find(hmenu);
        if (it != m_submenus_by_handle.end() && it->second) {
            targetMenu = (__bridge NSMenu*)it->second;
        }
        if (!targetMenu) {
            VITREO_TRACE( "[cocoa] append_menu_item: hmenu=0x%x not found\n", hmenu);
            return;
        }

        if (is_separator) {
            [targetMenu addItem:[NSMenuItem separatorItem]];
            return;
        }

        if (!vtr_menu_handlers()) {
            vtr_set_menu_handlers([[NSMutableArray alloc] init]);
        }

        // Convert UTF-8 std::string to NSString
        NSString* rawLabel = [NSString stringWithUTF8String:text_copy.c_str()];
        if (!rawLabel) rawLabel = @"(?)";
        NSString* label = strip_win_accelerator(rawLabel);

        // Strip shortcut suffix
        NSRange tabRange = [label rangeOfString:@"\t"];
        if (tabRange.location != NSNotFound) {
            label = [label substringToIndex:tabRange.location];
        } else {
            NSRange spaces = [label rangeOfString:@"   "];
            if (spaces.location != NSNotFound) {
                label = [[label substringToIndex:spaces.location]
                    stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
            }
        }

        VtrMenuItemHandler* handler = [[VtrMenuItemHandler alloc] init];
        handler.targetHwnd = target;
        handler.menuItemId = item_id;
        handler.queue = queue;
        [vtr_menu_handlers() addObject:handler];

        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:label
                                                      action:@selector(menuItemClicked:)
                                               keyEquivalent:@""];
        [item setTarget:handler];
        [item setTag:item_id];
        [item setEnabled:YES];
        [targetMenu addItem:item];

        // Track for CheckMenuItem/EnableMenuItem
        m_menu_items_by_id[item_id] = (__bridge_retained void*)item;

        VITREO_TRACE( "[cocoa] append_menu_item: hmenu=0x%x, id=%u, text=%s\n",
                     hmenu, item_id, text_copy.c_str());
    });
}

} // namespace vitreo::windowing
