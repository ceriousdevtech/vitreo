/// @file cocoa_bridge.mm
/// @brief Cocoa bridge implementation with real NSWindow (WINDOW-003)

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

// Declared in windowing/cocoa/objc_helpers.h — must stay out of the
// anonymous namespace above, which would give them internal linkage.

bool main_bundle_declares_application_icon() {
    NSBundle* bundle = [NSBundle mainBundle];
    if (!bundle) return false;

    id icon_file = [bundle objectForInfoDictionaryKey:@"CFBundleIconFile"];
    if ([icon_file isKindOfClass:[NSString class]] &&
        [(NSString*)icon_file length] != 0) {
        return true;
    }

    id icon_name = [bundle objectForInfoDictionaryKey:@"CFBundleIconName"];
    if ([icon_name isKindOfClass:[NSString class]] &&
        [(NSString*)icon_name length] != 0) {
        return true;
    }

    id icons = [bundle objectForInfoDictionaryKey:@"CFBundleIcons"];
    return [icons isKindOfClass:[NSDictionary class]] &&
           [(NSDictionary*)icons count] != 0;
}

void set_process_application_name(NSString* name) {
    if (!name || [name length] == 0) return;

    // AppKit reads this process/menu identity when it is first created.
    // LaunchServices gets the Dock label from the generated app bundle.
    CFBundleRef main_bundle = CFBundleGetMainBundle();
    if (main_bundle) {
        CFMutableDictionaryRef info_dict =
            (CFMutableDictionaryRef)CFBundleGetInfoDictionary(main_bundle);
        if (info_dict) {
            CFDictionarySetValue(info_dict,
                                 CFSTR("CFBundleName"),
                                 (__bridge CFStringRef)name);
        }
    }
    [[NSProcessInfo processInfo] setValue:name forKey:@"processName"];
}

// Shared with the cocoa_*.mm translation units through
// windowing/cocoa/objc_helpers.h, so it must have external linkage —
// it cannot live in the anonymous namespace above.
vitreo::u64 continuous_tick_count_ms() noexcept {
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t info{};
        (void)mach_timebase_info(&info);
        return info;
    }();
    const vitreo::u64 ticks = mach_continuous_time();
    return (ticks * timebase.numer / timebase.denom) / 1000000ULL;
}

// =============================================================================
// VtrAppActivationObserver — Cmd-Tab / deactivate exclusive fullscreen host
// =============================================================================

@interface VtrAppActivationObserver : NSObject <NSApplicationDelegate>
@property (nonatomic, assign) vitreo::windowing::CocoaBridge* bridge;
@end

@implementation VtrAppActivationObserver
- (void)appDidResignActive:(NSNotification*)notification {
    (void)notification;
    if (_bridge) _bridge->on_app_did_resign_active();
}
- (void)appDidBecomeActive:(NSNotification*)notification {
    (void)notification;
    if (_bridge) _bridge->on_app_did_become_active();
}
- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender
                    hasVisibleWindows:(BOOL)flag {
    (void)sender;
    // Dock click with every HWND orderOut: AppKit will not invent a window.
    // Re-apply ShowWindow visibility so a WS_VISIBLE guest HWND appears.
    if (_bridge && _bridge->route_dock_session()) return YES;
    if (_bridge) _bridge->on_app_did_become_active();
    // Nothing showing and nothing minimized: the app's notification-area icon.
    if (_bridge) _bridge->on_dock_reopen(flag == YES);
    return YES;
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    // [NSApp terminate:] would exit(0) the host without ExitProcess/WM_QUIT.
    // Dock Quit / Cmd+Q still have to reach the guest: post WM_CLOSE and
    // cancel host termination until begin_shutdown() (guest ExitProcess).
    if (_bridge && _bridge->shutting_down()) {
        return NSTerminateNow;
    }
    if (_bridge) {
        _bridge->request_guest_quit();
    }
    std::fprintf(stderr,
                 "[cocoa] applicationShouldTerminate pid=%d — guest quit "
                 "requested\n",
                 static_cast<int>(::getpid()));
    std::fflush(stderr);
    return NSTerminateCancel;
}
@end

namespace vitreo::windowing {

// =============================================================================
// NSApplication Bootstrap
// =============================================================================

void CocoaBridge::ensure_app_initialized() {
    if (m_app_initialized) return;

    VITREO_TRACE( "[cocoa] Initializing NSApplication...\n");

    // The Windows process lifetime is not the lifetime of its HWND set. A
    // guest may destroy its splash window, continue background work, and only
    // later create its real UI (possibly from another process in the same
    // ProcessSession). AppKit automatic termination bypasses
    // applicationShouldTerminate: and would otherwise exit the host during
    // that legitimate windowless interval. NSProcessInfo keeps this opt-out
    // counter even if automatic-termination support is enabled later while
    // NSApplication finishes launching.
    if (!m_automatic_termination_guard_active) {
        [[NSProcessInfo processInfo]
            disableAutomaticTermination:@"Vitreo guest process is active"];
        m_automatic_termination_guard_active = true;
    }

    // For CLI tools without an .app bundle, inject NSHighResolutionCapable
    // into the process info dictionary so macOS renders at native Retina
    // resolution instead of upscaling from 1x.
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    if (mainBundle) {
        CFMutableDictionaryRef infoDict =
            (CFMutableDictionaryRef)CFBundleGetInfoDictionary(mainBundle);
        if (infoDict) {
            CFDictionarySetValue(infoDict,
                                 CFSTR("NSHighResolutionCapable"),
                                 kCFBooleanTrue);
        }
    }

    [NSApplication sharedApplication];
    // Activation policy and finishLaunching are deferred to finish_app_launch()
    // so the guest name, menu, and icon are ready before the Dock tile appears.
    VITREO_TRACE( "[cocoa] NSApplication pre-initialized (activation + launch deferred)\n");

    m_app_initialized = true;
}

void CocoaBridge::finish_app_launch(const std::wstring& title) {
    if (m_app_launched) return;
    m_app_launched = true;

    // The generated app bundle supplies the stable executable-derived Dock
    // identity. The first guest window title is still useful for the initial
    // application menu.
    NSString* name = nil;
    if (!title.empty()) {
        name = [[NSString alloc] initWithBytes:title.data()
                                        length:title.size() * sizeof(wchar_t)
                                      encoding:NSUTF32LittleEndianStringEncoding];
        set_process_application_name(name);
    } else {
        // The first window can be an untitled helper (a hidden COM/IPC
        // window). The process still needs a main menu: an active
        // application without one leaves the previous application's menu
        // bar on screen, unresponsive, until something else activates.
        NSString* registered = [[NSRunningApplication currentApplication] localizedName];
        name = [(registered.length ? registered : [[NSProcessInfo processInfo] processName]) copy];
    }
    {
        // Install a minimal main menu so finishLaunching sees the correct app
        // menu title (replaced later by install_menu_bar).
        NSMenu* mainMenu = [[NSMenu alloc] initWithTitle:@"MainMenu"];
        NSMenuItem* appMenuItem = [[NSMenuItem alloc] initWithTitle:name
                                                            action:nil
                                                     keyEquivalent:@""];
        NSMenu* appMenu = [[NSMenu alloc] initWithTitle:name];
        NSString* quitTitle = [NSString stringWithFormat:@"Quit %@", name];
        NSMenuItem* quitItem = [[NSMenuItem alloc]
            initWithTitle:quitTitle
                   action:@selector(terminate:)
            keyEquivalent:@"q"];
        [appMenu addItem:quitItem];
        [appMenuItem setSubmenu:appMenu];
        [mainMenu addItem:appMenuItem];

        // Standard Edit menu so Cmd+V/C/X/A reach the first responder (native
        // NSTextField / field editor). Without this, paste often dies on the
        // flip content view's empty keyDown:.
        {
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

        vtr_ensure_enter_full_screen_menu(mainMenu);

        [NSApp setMainMenu:mainMenu];

        VITREO_TRACE( "[cocoa] Set initial app menu name to '%ls' before finishLaunching\n",
                     title.c_str());
    }

    // A declared bundle icon is authoritative. LaunchServices applies the
    // native macOS presentation to legacy ICO artwork and publishes one
    // consistent image to the Dock and Command-Tab. Only unbundled launches
    // need AppKit's dynamic application-icon fallback.
    const bool bundle_owns_icon = main_bundle_declares_application_icon();
    NSImage* launch_icon = nil;
    if (!bundle_owns_icon && !m_pending_icon.empty()) {
        NSData* ns_data = [NSData dataWithBytes:m_pending_icon.data()
                                         length:m_pending_icon.size()];
        NSImage* source_icon =
            [[[NSImage alloc] initWithData:ns_data] autorelease];
        launch_icon = create_macos_presented_app_icon(source_icon);
        if (!launch_icon) launch_icon = source_icon;
        if (launch_icon) {
            [NSApp setApplicationIconImage:launch_icon];
            VITREO_TRACE(
                "[cocoa] Primed native-presented fallback Dock icon before "
                "app activation (%zu bytes)\n",
                m_pending_icon.size());
        }
    }

    // Only the ProcessSession root owns a Dock/Command-Tab identity.
    // CreateProcess descendants are independent Windows processes, but on
    // macOS they are accessory applications within that originating session:
    // they may create and activate native windows without publishing a new
    // Dock tile for every updater, service, or browser-helper process.
    const bool is_child_process = macos_session_is_child_process();
    [NSApp setActivationPolicy:
        is_child_process
            ? NSApplicationActivationPolicyAccessory
            : NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];
    start_dock_session();
    // Accessory children must not steal session activation. A GPU or crash
    // helper that activateIgnoringOtherApps hides sibling windows of a
    // the guest (ShowWindow already displayed them in another
    // CreateProcess host).
    if (!is_child_process) {
        [NSApp activateIgnoringOtherApps:YES];
    }
    VITREO_TRACE(
        "[cocoa] activation policy=%s\n",
        is_child_process ? "accessory-child" : "regular-session-root");

    // Reapply the fallback after launch so an unbundled process's already
    // created Dock tile observes the guest icon.
    if (!m_pending_icon.empty()) {
        if (bundle_owns_icon) {
            VITREO_TRACE( "[cocoa] Using declared bundle icon for Dock and Command-Tab\n");
        } else if (launch_icon) {
            [NSApp setApplicationIconImage:launch_icon];
            VITREO_TRACE(
                "[cocoa] Applied native-presented fallback Dock icon "
                "(%zu bytes)\n",
                m_pending_icon.size());
        } else {
            VITREO_TRACE( "[cocoa] Deferred icon decode failed (%zu bytes)\n",
                         m_pending_icon.size());
        }
        m_pending_icon.clear();
    }

    VITREO_TRACE( "[cocoa] NSApplication finishLaunching complete\n");

    // Exclusive fullscreen sits above the menu bar. Without these observers it
    // stays there after Cmd-Tab and blocks every other app — not how macOS
    // fullscreen/exclusive games behave.
    if (!m_app_activation_observer) {
        VtrAppActivationObserver* obs = [[VtrAppActivationObserver alloc] init];
        obs.bridge = this;
        NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
        [nc addObserver:obs
               selector:@selector(appDidResignActive:)
                   name:NSApplicationDidResignActiveNotification
                 object:NSApp];
        [nc addObserver:obs
               selector:@selector(appDidBecomeActive:)
                   name:NSApplicationDidBecomeActiveNotification
                 object:NSApp];
        m_app_activation_observer = (__bridge_retained void*)obs;
        if ([NSApp delegate] == nil) {
            [NSApp setDelegate:obs];
        }
    }

    // While this app is active, macOS still routes mouse motion over another
    // app's window (a second display, the desktop) to that app. A global
    // monitor sees exactly those events and never ours, so nothing is counted
    // twice; the guest keeps its device motion as on Windows.
    if (!m_offwindow_motion_monitor) {
        CocoaBridge* self = this;
        const NSEventMask mask =
            NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged |
            NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged;
        id monitor = [NSEvent
            addGlobalMonitorForEventsMatchingMask:mask
                                          handler:^(NSEvent* event) {
                                              self->note_offwindow_mouse_motion(
                                                  (__bridge void*)event);
                                          }];
        m_offwindow_motion_monitor = (__bridge_retained void*)monitor;
    }
}

// =============================================================================
// Construction / Destruction
// =============================================================================

CocoaBridge::CocoaBridge() {
    // Launched by a process that could set the foreground (Learn
    // SetForegroundWindow: "started by the foreground process").
    const char* grant = std::getenv(kForegroundGrantEnv);
    m_holds_start_grant.store(grant != nullptr && std::strcmp(grant, "1") == 0,
                              std::memory_order_release);
}

CocoaBridge::~CocoaBridge() {
    stop_dock_session();
    // Frontends may already be destroyed. Tear down native monitors without
    // invoking their callbacks, then disable queued gesture notifications.
    if (m_owns_native_resize.load(std::memory_order_acquire))
        vtr_run_on_main_sync(^{ finish_native_window_resize(NULL_HWND, false); });
    m_move_size_callback = {};
    m_active_move_sizes.clear();
    // Balance only the hide call owned by this bridge. Process teardown must
    // not leave the shared macOS pointer hidden after a guest exits or faults.
    if (m_host_cursor_hide_applied) {
        CGDisplayShowCursor(kCGDirectMainDisplay);
        m_host_cursor_hide_applied = false;
    }
    if (m_cursor_clip_monitor) {
        id monitor = (__bridge_transfer id)m_cursor_clip_monitor;
        m_cursor_clip_monitor = nullptr;
        [NSEvent removeMonitor:monitor];
    }
    if (m_cursor_clip_global_monitor) {
        id monitor = (__bridge_transfer id)m_cursor_clip_global_monitor;
        m_cursor_clip_global_monitor = nullptr;
        [NSEvent removeMonitor:monitor];
    }
    if (m_offwindow_motion_monitor) {
        id monitor = (__bridge_transfer id)m_offwindow_motion_monitor;
        m_offwindow_motion_monitor = nullptr;
        [NSEvent removeMonitor:monitor];
    }
    if (m_app_activation_observer) {
        VtrAppActivationObserver* obs =
            (__bridge_transfer VtrAppActivationObserver*)m_app_activation_observer;
        m_app_activation_observer = nullptr;
        obs.bridge = nullptr;
        if ([NSApp delegate] == obs) {
            [NSApp setDelegate:nil];
        }
        [[NSNotificationCenter defaultCenter] removeObserver:obs];
    }
    if (m_custom_ns_cursor) {
        CFRelease((CFTypeRef)m_custom_ns_cursor);
        m_custom_ns_cursor = nullptr;
        m_cursor_shape_gen.fetch_add(1, std::memory_order_release);
    }

    // Prefer the main-thread path used by live DestroyWindow. Falling back to
    // a bare [close] here races AppKit when the host process stays alive for
    // ProcessSession::wait_for_workers after the guest has already exited.
    destroy_all_windows();

    for (auto& [hbm, img_ptr] : m_bitmaps) {
        if (img_ptr) {
            CFRelease((CFTypeRef)img_ptr);
            img_ptr = nullptr;
        }
    }
    m_bitmaps.clear();

    // Balance the process-wide NSProcessInfo counter only after native
    // teardown is complete. Re-enabling it from begin_shutdown() would allow
    // AppKit to kill the host while ProcessSession is still joining children.
    if (m_automatic_termination_guard_active) {
        [[NSProcessInfo processInfo]
            enableAutomaticTermination:@"Vitreo guest process is active"];
        m_automatic_termination_guard_active = false;
    }
}

void CocoaBridge::begin_shutdown() noexcept {
    m_shutdown.store(true, std::memory_order_release);
    stop_dock_session();
    // Wake the main run loop so any worker blocked in vtr_run_on_main_sync
    // can observe the bailout flag and return.
    if (m_app_initialized) {
        CFRunLoopWakeUp(CFRunLoopGetMain());
    }
}

void CocoaBridge::request_guest_quit() {
    vtr_run_on_main_sync(^{
        broadcast_dock_session_quit();
        if (!m_queue) return;
        bool posted_close = false;
        for (const auto& [hwnd, state] : m_windows) {
            if (!state.ns_window) continue;
            if (state.guest_style & WS::CHILD) continue;
            MSG msg{};
            msg.hwnd = hwnd;
            msg.message = WM::CLOSE;
            m_queue->post(msg);
            posted_close = true;
        }
        if (!posted_close) {
            m_queue->post_quit(0);
        }
    });
}

} // namespace vitreo::windowing
