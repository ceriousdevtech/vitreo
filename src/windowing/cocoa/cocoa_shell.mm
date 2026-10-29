// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_shell.mm
/// @brief Shell services: clipboard, file dialogs, and shell execute.
///
/// The host-facing services a guest reaches through Win32 that are neither
/// windowing nor input: NSPasteboard behind the clipboard APIs,
/// NSOpenPanel/NSSavePanel behind the common file dialogs, and NSWorkspace
/// behind ShellExecute.
///
/// Split out of cocoa_bridge.mm, which was 8,675 lines. These are CocoaBridge
/// methods defined in a separate translation unit; the class is unchanged.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CATransaction.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAShapeLayer.h>
#import <objc/runtime.h>
#include "windowing/cocoa/objc_helpers.h"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/cocoa/app_icon_presentation.hpp"
#include <cstdio>
#include <spawn.h>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>

extern char** environ;
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <algorithm>
#include <CoreFoundation/CFRunLoop.h>
#include <CoreGraphics/CGEventSource.h>
#include <mach/mach_time.h>

/// Keeps an Open/Save panel inside the capsule's drives. Items outside are
/// disabled (folders above a drive root stay enabled so the user can navigate
/// down to it), and a choice outside is refused with an error the panel shows
/// as its own alert, leaving the panel open.
@interface VtrCapsulePanelDelegate : NSObject <NSOpenSavePanelDelegate>
- (instancetype)initWithRoots:(NSArray<NSString*>*)roots;
@end

@implementation VtrCapsulePanelDelegate {
    NSArray<NSString*>* _roots;
}

- (instancetype)initWithRoots:(NSArray<NSString*>*)roots {
    if ((self = [super init])) _roots = [roots copy];
    return self;
}

static NSString* vtr_resolved_path(NSURL* url) {
    return [[[url URLByResolvingSymlinksInPath] path] stringByStandardizingPath];
}

- (BOOL)isInside:(NSString*)path {
    for (NSString* root in _roots) {
        if ([path isEqualToString:root] ||
            [path hasPrefix:[root stringByAppendingString:@"/"]]) {
            return YES;
        }
    }
    return NO;
}

- (BOOL)isAncestorOfRoot:(NSString*)path {
    NSString* prefix = [path hasSuffix:@"/"] ? path : [path stringByAppendingString:@"/"];
    for (NSString* root in _roots) {
        if ([root hasPrefix:prefix]) return YES;
    }
    return NO;
}

- (BOOL)panel:(id)sender shouldEnableURL:(NSURL*)url {
    NSString* path = vtr_resolved_path(url);
    return [self isInside:path] || [self isAncestorOfRoot:path];
}

- (BOOL)panel:(id)sender validateURL:(NSURL*)url error:(NSError**)outError {
    if ([self isInside:vtr_resolved_path(url)]) return YES;
    if (outError) {
        NSString* app = [[NSProcessInfo processInfo] processName];
        *outError = [NSError
            errorWithDomain:NSCocoaErrorDomain
                       code:NSFileReadNoPermissionError
                   userInfo:@{
                       NSLocalizedDescriptionKey: [NSString stringWithFormat:
                           @"“%@” isn’t available to %@.", [url lastPathComponent], app],
                       NSLocalizedRecoverySuggestionErrorKey:
                           @"This app runs in its own container and can only open items inside it. "
                            "Choose a location inside the app’s drives.",
                   }];
    }
    return NO;
}
@end

namespace vitreo::windowing {

// =============================================================================
// Native Delegation: Clipboard (NSPasteboard)
// =============================================================================

bool CocoaBridge::clipboard_set_text(const std::string& utf8_text) {
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        NSString* str = [NSString stringWithUTF8String:utf8_text.c_str()];
        return [pb setString:str forType:NSPasteboardTypeString];
    }
}

std::string CocoaBridge::clipboard_get_text() {
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        NSString* str = [pb stringForType:NSPasteboardTypeString];
        if (str) {
            return std::string([str UTF8String]);
        }
        return {};
    }
}

i64 CocoaBridge::clipboard_change_count() {
    @autoreleasepool {
        return static_cast<i64>([[NSPasteboard generalPasteboard] changeCount]);
    }
}

void CocoaBridge::clipboard_clear() {
    @autoreleasepool {
        [[NSPasteboard generalPasteboard] clearContents];
    }
}


// =============================================================================
// Native Delegation: File Dialogs (NSOpenPanel / NSSavePanel)
// =============================================================================

CocoaBridge::FileDialogResult CocoaBridge::show_open_file_dialog(
    const std::string& initial_dir,
    const std::string& filter,
    const std::string& title) {
    if (![NSThread isMainThread]) {
        __block FileDialogResult result;
        vtr_run_on_main_sync(^{
            result = show_open_file_dialog(initial_dir, filter, title);
        });
        return result;
    }
    FileDialogResult result;
    @autoreleasepool {
        ensure_app_initialized();

        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];

        if (!title.empty()) {
            [panel setTitle:[NSString stringWithUTF8String:title.c_str()]];
        }

        if (!initial_dir.empty()) {
            // Use home directory as base for sandboxed path
            NSString* dir = [NSString stringWithUTF8String:initial_dir.c_str()];
            [panel setDirectoryURL:[NSURL fileURLWithPath:dir]];
        }

        // Parse filter for file types (e.g., "*.txt;*.doc" → ["txt", "doc"])
        if (!filter.empty() && filter != "*.*") {
            NSMutableArray<NSString*>* types = [NSMutableArray array];
            // Simple parser: extract extensions from patterns like "*.ext"
            std::string f = filter;
            size_t pos = 0;
            while ((pos = f.find("*.")) != std::string::npos) {
                f = f.substr(pos + 2);
                size_t end = f.find_first_of(";|,");
                std::string ext = (end != std::string::npos) ? f.substr(0, end) : f;
                if (!ext.empty() && ext != "*") {
                    [types addObject:[NSString stringWithUTF8String:ext.c_str()]];
                }
                if (end == std::string::npos) break;
                f = f.substr(end + 1);
            }
            if ([types count] > 0) {
                [panel setAllowedFileTypes:types];
            }
        }

        NSModalResponse resp = [panel runModal];
        if (resp == NSModalResponseOK && [[panel URLs] count] > 0) {
            NSString* path = [[[panel URLs] firstObject] path];
            result.ok = true;
            result.selected_path = std::string([path UTF8String]);
        }
    }
    return result;
}

CocoaBridge::ItemDialogResult CocoaBridge::show_item_dialog(
    const ItemDialogRequest& request) {
    if (![NSThread isMainThread]) {
        __block ItemDialogResult result;
        vtr_run_on_main_sync(^{
            result = show_item_dialog(request);
        });
        return result;
    }
    ItemDialogResult result;
    @autoreleasepool {
        ensure_app_initialized();
        NSSavePanel* panel = nil;
        if (request.save) {
            panel = [NSSavePanel savePanel];
            if (!request.file_name.empty()) {
                [panel setNameFieldStringValue:
                    [NSString stringWithUTF8String:request.file_name.c_str()]];
            }
            if (!request.file_name_label.empty()) {
                [panel setNameFieldLabel:
                    [NSString stringWithUTF8String:request.file_name_label.c_str()]];
            }
        } else {
            NSOpenPanel* open = [NSOpenPanel openPanel];
            [open setCanChooseFiles:!request.pick_folders];
            [open setCanChooseDirectories:request.pick_folders];
            [open setAllowsMultipleSelection:request.multi_select];
            [open setResolvesAliases:YES];
            panel = open;
        }
        [panel setCanCreateDirectories:YES];
        [panel setShowsHiddenFiles:request.show_hidden];
        if (!request.title.empty()) {
            // Modern panels have no title bar; the title is the message line.
            NSString* title = [NSString stringWithUTF8String:request.title.c_str()];
            [panel setTitle:title];
            [panel setMessage:title];
        }
        if (!request.ok_label.empty()) {
            [panel setPrompt:[NSString stringWithUTF8String:request.ok_label.c_str()]];
        }
        if (!request.initial_dir.empty()) {
            [panel setDirectoryURL:[NSURL fileURLWithPath:
                [NSString stringWithUTF8String:request.initial_dir.c_str()]]];
        }
        NSMutableArray<NSString*>* types = [NSMutableArray array];
        bool any_type = request.extensions.empty();
        for (const auto& extension : request.extensions) {
            if (extension.empty() || extension == "*") { any_type = true; break; }
            [types addObject:[NSString stringWithUTF8String:extension.c_str()]];
        }
        if (!any_type && [types count] > 0 && !request.pick_folders) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [panel setAllowedFileTypes:types];
#pragma clang diagnostic pop
        }
        VtrCapsulePanelDelegate* capsule_delegate = nil;
        if (!request.allowed_roots.empty()) {
            NSMutableArray<NSString*>* roots = [NSMutableArray array];
            for (const auto& root : request.allowed_roots) {
                NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:root.c_str()]];
                [roots addObject:vtr_resolved_path(url)];
            }
            capsule_delegate = [[VtrCapsulePanelDelegate alloc] initWithRoots:roots];
            [panel setDelegate:capsule_delegate];
        }
        const NSModalResponse response = [panel runModal];
        [panel setDelegate:nil];
        if (response == NSModalResponseOK) {
            if (request.save) {
                if (NSURL* url = [panel URL]) {
                    result.paths.emplace_back([[url path] UTF8String]);
                }
            } else {
                for (NSURL* url in [(NSOpenPanel*)panel URLs]) {
                    result.paths.emplace_back([[url path] UTF8String]);
                }
            }
            result.ok = !result.paths.empty();
        }
    }
    return result;
}

CocoaBridge::FileDialogResult CocoaBridge::show_save_file_dialog(
    const std::string& initial_dir,
    const std::string& default_name,
    const std::string& filter,
    const std::string& title) {
    if (![NSThread isMainThread]) {
        __block FileDialogResult result;
        vtr_run_on_main_sync(^{
            result = show_save_file_dialog(initial_dir, default_name, filter, title);
        });
        return result;
    }
    FileDialogResult result;
    @autoreleasepool {
        ensure_app_initialized();

        NSSavePanel* panel = [NSSavePanel savePanel];
        [panel setCanCreateDirectories:YES];

        if (!title.empty()) {
            [panel setTitle:[NSString stringWithUTF8String:title.c_str()]];
        }

        if (!default_name.empty()) {
            [panel setNameFieldStringValue:[NSString stringWithUTF8String:default_name.c_str()]];
        }

        if (!initial_dir.empty()) {
            NSString* dir = [NSString stringWithUTF8String:initial_dir.c_str()];
            [panel setDirectoryURL:[NSURL fileURLWithPath:dir]];
        }

        if (!filter.empty() && filter != "*.*") {
            NSMutableArray<NSString*>* types = [NSMutableArray array];
            std::string f = filter;
            size_t pos = 0;
            while ((pos = f.find("*.")) != std::string::npos) {
                f = f.substr(pos + 2);
                size_t end = f.find_first_of(";|,");
                std::string ext = (end != std::string::npos) ? f.substr(0, end) : f;
                if (!ext.empty() && ext != "*") {
                    [types addObject:[NSString stringWithUTF8String:ext.c_str()]];
                }
                if (end == std::string::npos) break;
                f = f.substr(end + 1);
            }
            if ([types count] > 0) {
                [panel setAllowedFileTypes:types];
            }
        }

        NSModalResponse resp = [panel runModal];
        if (resp == NSModalResponseOK && [panel URL]) {
            NSString* path = [[panel URL] path];
            result.ok = true;
            result.selected_path = std::string([path UTF8String]);
        }
    }
    return result;
}

// =============================================================================
// Native Delegation: Shell Execute (NSWorkspace)
// =============================================================================

// std::isalnum
#include <cctype>

HostHttpHandler host_http_handler() {
    HostHttpHandler handler;
    @autoreleasepool {
        NSURL* probe = [NSURL URLWithString:@"https://example.com"];
        if (probe == nil) return handler;
        NSURL* app = [[NSWorkspace sharedWorkspace] URLForApplicationToOpenURL:probe];
        if (app == nil) return handler;
        NSString* display = [[NSFileManager defaultManager] displayNameAtPath:app.path];
        if (display == nil || display.length == 0) return handler;
        NSString* trimmed = display;
        if ([trimmed hasSuffix:@".app"]) {
            trimmed = [trimmed substringToIndex:trimmed.length - 4];
        }
        NSBundle* bundle = [NSBundle bundleWithURL:app];
        NSString* exe = bundle.executablePath;
        if (exe == nil || exe.length == 0) return handler;
        handler.display_name = std::string(trimmed.UTF8String);
        handler.host_executable = std::string(exe.UTF8String);
        handler.guest_executable =
            "C:\\Program Files\\" + handler.display_name + "\\" +
            handler.display_name + ".exe";
        handler.command = "\"" + handler.guest_executable + "\" -- \"%1\"";
    }
    return handler;
}

bool launch_host_http_handler(const std::string& guest_executable,
                                 const std::string& parameters) {
    const HostHttpHandler handler = host_http_handler();
    if (handler.host_executable.empty() || handler.guest_executable.empty()) {
        return false;
    }
    std::string wanted = guest_executable;
    std::string reported = handler.guest_executable;
    const auto lower = [](std::string& s) {
        for (char& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    };
    lower(wanted);
    lower(reported);
    if (wanted != reported) return false;

    std::vector<std::string> stored;
    std::string cur;
    bool quoted = false;
    for (char c : parameters) {
        if (c == '"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && (c == ' ' || c == '\t')) {
            if (!cur.empty()) {
                stored.push_back(std::move(cur));
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) stored.push_back(std::move(cur));

    std::string bin = handler.host_executable;
    std::vector<char*> argv;
    argv.push_back(bin.data());
    for (auto& arg : stored) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, bin.c_str(), nullptr, nullptr, argv.data(),
                               environ);
    std::fprintf(stderr,
                 "[vitreo] ShellExecute host browser '%s' (%s) params='%s' -> %s\n",
                 handler.display_name.c_str(), bin.c_str(), parameters.c_str(),
                 rc == 0 ? "spawned" : "FAILED");
    if (rc != 0) return false;
    std::thread([pid] {
        int status = 0;
        (void)waitpid(pid, &status, 0);
    }).detach();
    return true;
}

bool CocoaBridge::shell_open(const std::string& target) {
    if (target.empty()) return false;

    // ShellExecute is called on a guest thread that may be holding a lock and
    // may not return to the AppKit run loop. Queueing NSWorkspace on the main
    // queue left the open sitting there. One guest reported
    // that the redirect had started, and the Mac browser never appeared.
    //
    // /usr/bin/open asks LaunchServices and returns. Spawn it off the guest
    // thread and reap it so the verb stays asynchronous, matching ShellExecute.
    std::thread([target] {
        std::string arg = target;
        char* argv[] = {const_cast<char*>("/usr/bin/open"), arg.data(), nullptr};
        pid_t pid = 0;
        const int rc = posix_spawn(&pid, "/usr/bin/open", nullptr, nullptr,
                                   argv, environ);
        std::fprintf(stderr, "[vitreo] shell_open '%s' -> %s\n",
                     target.c_str(), rc == 0 ? "spawned" : "FAILED");
        if (rc == 0) {
            int status = 0;
            (void)waitpid(pid, &status, 0);
        }
    }).detach();
    return true;
}
} // namespace vitreo::windowing
