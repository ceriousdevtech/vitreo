// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_message_box.mm
/// @brief MessageBox and TaskDialog shown as NSAlert.
///
/// The text, buttons and icon of a Win32 message box or task dialog mapped
/// onto an NSAlert, run modally, with the user's choice returned to the
/// caller.
///
/// Split out of cocoa_bridge.mm, which was 7,624 lines. These are CocoaBridge
/// methods defined in a separate translation unit; the class is unchanged.

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

namespace vitreo::windowing {

int CocoaBridge::show_task_dialog(
    const std::string& title,
    const std::string& instruction,
    const std::string& content,
    const std::vector<TaskDialogButton>& buttons,
    int default_button,
    const std::string& verification_text,
    bool& verification_checked,
    bool warning) {
    if (buttons.empty()) return 0;

    __block int result = buttons.front().id;
    __block bool checked = verification_checked;

    vtr_run_on_main_sync(^{
        ensure_app_initialized();
        @autoreleasepool {
            NSAlert* alert = [[NSAlert alloc] init];
            NSString* (^from_guest)(const std::string&) = ^(const std::string& raw) {
                NSString* str = [NSString stringWithUTF8String:raw.c_str()];
                if (str == nil) {
                    str = [[NSString alloc]
                        initWithBytes:raw.data()
                               length:raw.size()
                             encoding:NSWindowsCP1252StringEncoding];
                }
                return str != nil ? str : @"";
            };

            // A task dialog's heading is the main instruction; the window
            // title is only chrome, and macOS alerts have no title bar, so the
            // title is folded in only when there is no instruction to show.
            const std::string& heading =
                !instruction.empty() ? instruction : title;
            [alert setMessageText:from_guest(heading)];
            [alert setInformativeText:from_guest(content)];
            [alert setAlertStyle:warning ? NSAlertStyleCritical
                                         : NSAlertStyleInformational];

            // Button and checkbox labels carry Win32 mnemonics ("&Yes",
            // "&&" for a literal ampersand); macOS buttons have none.
            for (const auto& button : buttons) {
                [alert addButtonWithTitle:strip_win_accelerator(from_guest(button.label))];
            }
            if (default_button > 0 &&
                default_button < static_cast<int>(buttons.size())) {
                // NSAlert makes the first button the default; moving the key
                // equivalent is how another one becomes it.
                NSArray<NSButton*>* views = [alert buttons];
                [[views objectAtIndex:0] setKeyEquivalent:@""];
                [[views objectAtIndex:
                    static_cast<NSUInteger>(default_button)]
                    setKeyEquivalent:@"\r"];
            }

            if (!verification_text.empty()) {
                [alert setShowsSuppressionButton:YES];
                [[alert suppressionButton]
                    setTitle:strip_win_accelerator(from_guest(verification_text))];
                [[alert suppressionButton]
                    setState:checked ? NSControlStateValueOn
                                     : NSControlStateValueOff];
            }

            // A full-screen guest window sits above the menu bar and would
            // cover the alert; drop it back to normal level for the duration.
            NSWindow* fs = nil;
            for (auto& [hwnd, state] : m_windows) {
                (void)hwnd;
                if (state.fullscreen && state.ns_window) {
                    fs = reinterpret_cast<NSWindow*>(state.ns_window);
                    break;
                }
            }
            const NSInteger saved_level = fs ? [fs level] : 0;
            if (fs) [fs setLevel:NSNormalWindowLevel];
            const bool was_hidden = m_cursor_hidden;
            if (was_hidden) set_cursor_visible(true);

            [NSApp activateIgnoringOtherApps:YES];
            const NSModalResponse response = [alert runModal];

            if (fs) [fs setLevel:saved_level];
            if (was_hidden) set_cursor_visible(false);

            if (!verification_text.empty()) {
                checked = [[alert suppressionButton] state] ==
                          NSControlStateValueOn;
            }
            const NSInteger idx = response - NSAlertFirstButtonReturn;
            result = (idx >= 0 && idx < static_cast<NSInteger>(buttons.size()))
                         ? buttons[static_cast<usize>(idx)].id
                         : buttons.front().id;
        }
    });

    verification_checked = checked;
    VITREO_TRACE("[cocoa] TaskDialog [%s] buttons=%zu -> %d checked=%d\n",
                 title.c_str(), buttons.size(), result, checked ? 1 : 0);
    return result;
}

int CocoaBridge::show_message_box(const std::string& caption,
                                  const std::string& text,
                                  unsigned int type) {
    // Win32 button-set and return-code constants (see winuser.h).
    constexpr int IDOK = message_box::IDOK, IDCANCEL = message_box::IDCANCEL;

    __block int result = IDOK;

    vtr_run_on_main_sync(^{
        ensure_app_initialized();
        @autoreleasepool {
            NSAlert* alert = [[NSAlert alloc] init];
            // stringWithUTF8String: returns nil for any byte sequence that is
            // not valid UTF-8 -- and MessageBoxA text is ANSI (CP-1252), so
            // e.g. the TM sign 0x99 (CP-1252) made this nil, and
            // setMessageText:nil throws NSInvalidArgumentException, killing
            // the whole host process over one dialog. Try UTF-8 (the W path
            // marshals through it), fall back to CP-1252 (the A path's real
            // encoding), never pass nil.
            NSString* (^from_guest)(const std::string&) = ^(const std::string& raw) {
                NSString* str = [NSString stringWithUTF8String:raw.c_str()];
                if (str == nil) {
                    str = [[NSString alloc]
                        initWithBytes:raw.data()
                               length:raw.size()
                             encoding:NSWindowsCP1252StringEncoding];
                }
                return str != nil ? str : @"";
            };
            [alert setMessageText:from_guest(caption)];
            [alert setInformativeText:from_guest(text)];

            // NSAlert returns NSAlertFirstButtonReturn for the button added
            // first, so add them in Win32's left-to-right order and map back
            // by index. Button set, MB_DEFBUTTONn and the icon come from
            // message_box::layout (winuser.h values, Learn semantics).
            const auto layout = message_box::layout(type);
            int ids[4] = {IDOK, 0, 0, 0};
            int count = 0;
            for (u32 i = 0; i < layout.count; ++i) {
                if (layout.buttons[i].id == message_box::IDHELP_BUTTON) {
                    // MB_HELP: NSAlert's help button (it does not close the
                    // alert, matching Win32 where Help only sends WM_HELP).
                    alert.showsHelp = YES;
                    continue;
                }
                NSButton* button = [alert addButtonWithTitle:
                    [NSString stringWithUTF8String:layout.buttons[i].title]];
                ids[count] = layout.buttons[i].id;
                if (layout.buttons[i].id == IDCANCEL) {
                    button.keyEquivalent = @"\033";
                }
                ++count;
            }
            // Return goes to the MB_DEFBUTTONn button (NSAlert gives it to
            // the first button by default).
            if (layout.buttons[layout.default_index].id !=
                message_box::IDHELP_BUTTON) {
                NSArray<NSButton*>* all = alert.buttons;
                for (NSButton* b in all) {
                    if ([b.keyEquivalent isEqualToString:@"\r"]) b.keyEquivalent = @"";
                }
                all[layout.default_index].keyEquivalent = @"\r";
            }
            switch (layout.icon) {
            case message_box::Icon::Error:
            case message_box::Icon::Warning:
                alert.alertStyle = NSAlertStyleCritical; break;
            case message_box::Icon::Information:
            case message_box::Icon::Question:
                alert.alertStyle = NSAlertStyleInformational; break;
            case message_box::Icon::None:
                alert.alertStyle = NSAlertStyleWarning; break;
            }

            // A full-screen guest window sits above the menu bar and would
            // cover the alert; drop it back to normal level for the duration.
            NSWindow* fs = nil;
            for (auto& [hwnd, state] : m_windows) {
                (void)hwnd;
                if (state.fullscreen && state.ns_window) {
                    fs = reinterpret_cast<NSWindow*>(state.ns_window);
                    break;
                }
            }
            const NSInteger saved_level = fs ? [fs level] : 0;
            if (fs) [fs setLevel:NSNormalWindowLevel];
            const bool was_hidden = m_cursor_hidden;
            if (was_hidden) {
                set_cursor_visible(true);
            }

            [NSApp activateIgnoringOtherApps:YES];
            const NSModalResponse response = [alert runModal];

            if (fs) [fs setLevel:saved_level];
            if (was_hidden) {
                set_cursor_visible(false);
            }

            const NSInteger idx = response - NSAlertFirstButtonReturn;
            result = (idx >= 0 && idx < count) ? ids[idx] : ids[0];
        }
    });

    VITREO_TRACE("[cocoa] MessageBox [%s] type=0x%x -> %d\n",
                 caption.c_str(), type, result);
    return result;
}

} // namespace vitreo::windowing
