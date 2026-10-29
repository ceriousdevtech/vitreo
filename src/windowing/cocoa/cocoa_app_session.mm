// SPDX-License-Identifier: LGPL-2.1-only
/// One AppKit presentation identity for an independently executing guest family.
#import <Cocoa/Cocoa.h>
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/cocoa/window_presentation.hpp"
#include "windowing/cocoa/dock_session_election.hpp"
#include "ipc/macos_app_identity.hpp"
#include "ipc/hwnd_registry.hpp"
#include <libproc.h>
#include <mach/mach_time.h>
#include <unistd.h>
#include <map>

namespace {
constexpr NSString* kSessionMessage = @"org.vitreo.presentation.session";
using Member = vitreo::windowing::DockSessionMember;
uint64_t process_birth(pid_t pid) {
    proc_bsdinfo info{};
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info))
        return 0;
    return info.pbi_start_tvsec * 1000000ULL + info.pbi_start_tvusec;
}
}

@interface VtrDockSession : NSObject {
@public
    vitreo::windowing::CocoaBridge* bridge;
    std::recursive_mutex bridge_mutex;
    NSString* identity;
    std::string group;  // this process's AppUserModelID group
    NSTimer* timer;
    std::map<pid_t, Member> members;
    uint64_t birth;
    uint64_t active;
    NSTimeInterval joined;
    bool root;
    bool routing;
    bool quitting;
    pid_t before_activation;
    uint64_t before_activation_birth;
    NSTimeInterval activation_time;
}
- (void)receive:(NSNotification*)notification;
- (void)willActivate:(NSNotification*)notification;
- (void)publish:(NSString*)kind;
- (void)refresh;
- (void)stop;
- (pid_t)owner;
- (pid_t)target;
@end

@implementation VtrDockSession
- (void)willActivate:(NSNotification*)notification {
    (void)notification;
    std::lock_guard lock(bridge_mutex);
    if (!bridge || [self owner] != getpid()) return;
    before_activation = [self target];
    before_activation_birth = before_activation ? members.at(before_activation).birth : 0;
    activation_time = [NSDate timeIntervalSinceReferenceDate];
}
- (void)publish:(NSString*)kind {
    std::lock_guard lock(bridge_mutex);
    if (!bridge) return;
    const uint32_t hwnd = bridge->dock_session_target();
    const bool tray = bridge->dock_session_has_tray_target();
    group = bridge->dock_session_group(hwnd);
    members[getpid()] = {birth, active, hwnd, root, tray, group};
    [[NSDistributedNotificationCenter defaultCenter]
        postNotificationName:kSessionMessage object:identity
        userInfo:@{@"kind":kind, @"pid":@(getpid()), @"birth":@(birth),
                   @"active":@(active), @"hwnd":@(hwnd), @"root":@(root), @"tray":@(tray),
                   @"group":[NSString stringWithUTF8String:group.c_str()] ?: @""}
        deliverImmediately:YES];
}
- (void)receive:(NSNotification*)notification {
    std::lock_guard lock(bridge_mutex);
    if (!bridge) return;
    NSDictionary* info = notification.userInfo;
    const pid_t pid = [info[@"pid"] intValue];
    const uint64_t observed_birth = [info[@"birth"] unsignedLongLongValue];
    if (pid <= 0 || observed_birth == 0 || process_birth(pid) != observed_birth)
        return;
    NSString* kind = info[@"kind"];
    if (getenv("VITREO_DOCK_SESSION_TRACE")) fprintf(stderr, "[dock] recv self=%d from=%d kind=%s root=%d hwnd=%u group=%s\n", getpid(), pid, kind.UTF8String, [info[@"root"] boolValue], [info[@"hwnd"] unsignedIntValue], [info[@"group"] UTF8String] ?: "");
    NSString* sender_group = info[@"group"];
    const std::string observed_group = sender_group ? sender_group.UTF8String : "";
    if ([kind isEqualToString:@"quit"]) {
        // Dock Quit closes the windows of one taskbar group, not the family.
        if (pid != getpid() && members.contains(pid) && observed_group == group) {
            quitting = true;
            bridge->request_guest_quit();
            quitting = false;
        }
        return;
    }
    if ([kind isEqualToString:@"activate"]) {
        if ([info[@"target"] intValue] == getpid() &&
            [info[@"targetBirth"] unsignedLongLongValue] == birth) {
            bridge->activate_dock_session_target();
        }
        return;
    }
    if ([kind isEqualToString:@"leave"]) {
        members.erase(pid);
    } else {
        members[pid] = {observed_birth,
            [info[@"active"] unsignedLongLongValue],
            [info[@"hwnd"] unsignedIntValue], [info[@"root"] boolValue],
            [info[@"tray"] boolValue], observed_group};
    }
    if (pid != getpid() && [kind isEqualToString:@"hello"])
        [self publish:@"state"];
    [self refresh];
}
- (pid_t)owner {
    return vitreo::windowing::dock_session_group_owner(members, group);
}
- (pid_t)target {
    return vitreo::windowing::dock_session_group_target(members, group);
}
- (void)refresh {
    std::lock_guard lock(bridge_mutex);
    if (!bridge) return;
    std::erase_if(members, [](const auto& entry) {
        return process_birth(entry.first) != entry.second.birth;
    });
    // Give a newly started child time to hear the existing owner's reply.
    // Crash detection uses OS process birth identity, never heartbeat expiry.
    const bool may_own = root || [NSDate timeIntervalSinceReferenceDate] - joined >= 2;
    const bool owns = may_own && [self owner] == getpid();
    const auto policy = owns ? NSApplicationActivationPolicyRegular
                             : NSApplicationActivationPolicyAccessory;
    if ([NSApp activationPolicy] != policy && ![NSApp setActivationPolicy:policy])
        fprintf(stderr, "[vitreo] AppKit refused session Dock ownership change\n");
}
- (void)stop {
    // Relinquish before notifying the successor, so handoff cannot publish
    // two icons while the retiring guest's host is still waiting on workers.
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [[NSDistributedNotificationCenter defaultCenter]
        postNotificationName:kSessionMessage object:identity
        userInfo:@{@"kind":@"leave", @"pid":@(getpid()), @"birth":@(birth)}
        deliverImmediately:YES];
    [timer invalidate];
    [timer release];
    timer = nil;
    [[NSDistributedNotificationCenter defaultCenter] removeObserver:self];
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}
- (void)dealloc {
    [identity release];
    [super dealloc];
}
@end

namespace vitreo::windowing {

HWND CocoaBridge::dock_session_target() const {
    HWND fallback = NULL_HWND;
    const HWND preferred = m_preferred_key_hwnd.load(std::memory_order_acquire);
    for (const auto& [hwnd, state] : m_windows) {
        if (!state.ns_window || !state.visible || state.empty_window_rgn ||
            !guest_rect_intersects_any_monitor(state.frame) ||
            composition_host_defers_to_owned(state)) continue;
        // Owned dialogs may be the current interaction, but tool palettes and
        // hidden helper windows must never be invented by a Dock click.
        const NativeWindowState* identity_state = &state;
        bool peer_dialog = false;
        for (int depth = 0; identity_state->owner && depth < 64; ++depth) {
            const auto owner = m_windows.find(identity_state->owner);
            if (owner == m_windows.end()) {
                VtrDockSession* session = (VtrDockSession*)m_dock_session.load();
                if (state.is_dialog && session) {
                    const auto peer = ipc::hwnd_registry_lookup(identity_state->owner);
                    peer_dialog = peer && session->members.contains(peer->owner_host_pid);
                }
                break;
            }
            identity_state = &owner->second;
        }
        if (!peer_dialog && !presentation_has_taskbar_identity(make_presentation_signals(
                identity_state->guest_style, identity_state->guest_ex_style,
                identity_state->is_dialog, identity_state->client_drawn_frame,
                identity_state->owner != NULL_HWND, false,
                identity_state->fullscreen))) continue;
        if (state.guest_ex_style & (WS_EX::TOOLWINDOW | WS_EX::NOACTIVATE)) continue;
        if (hwnd == preferred) return hwnd;
        if (!fallback || hwnd < fallback) fallback = hwnd;
    }
    return fallback;
}

void CocoaBridge::start_dock_session() {
    if (m_dock_session) return;
    VtrDockSession* session = [VtrDockSession new];
    session->bridge = this;
    session->identity = [[NSString stringWithUTF8String:ipc::macos_app_identity().c_str()] copy];
    session->birth = process_birth(getpid());
    session->root = !macos_session_is_child_process();
    session->joined = [NSDate timeIntervalSinceReferenceDate];
    session->group = dock_session_group(NULL_HWND);
    m_dock_session = session;
    if (getenv("VITREO_DOCK_SESSION_TRACE")) fprintf(stderr, "[dock] join pid=%d birth=%llu root=%d id=%s group=%s\n", getpid(), (unsigned long long)session->birth, session->root, session->identity.UTF8String, session->group.c_str());
    [[NSDistributedNotificationCenter defaultCenter]
        addObserver:session selector:@selector(receive:) name:kSessionMessage
        object:session->identity suspensionBehavior:NSNotificationSuspensionBehaviorDeliverImmediately];
    [[NSNotificationCenter defaultCenter] addObserver:session
        selector:@selector(willActivate:) name:NSApplicationWillBecomeActiveNotification
        object:NSApp];
    // Process death (including SIGKILL) has no notification. A low-frequency
    // AppKit timer elects a surviving window owner and reannounces membership.
    session->timer = [[NSTimer timerWithTimeInterval:1 repeats:YES block:^(NSTimer*) {
        std::lock_guard lock(session->bridge_mutex);
        if (session->bridge) {
            [session publish:@"state"];
            [session refresh];
        }
    }] retain];
    [[NSRunLoop mainRunLoop] addTimer:session->timer forMode:NSRunLoopCommonModes];
    [session publish:@"hello"];
    [session refresh];
}

void CocoaBridge::stop_dock_session() {
    VtrDockSession* session = (VtrDockSession*)m_dock_session.exchange(nullptr);
    if (!session) return;
    {
        // A callback may be finishing on AppKit. Detach its raw pointer before
        // returning to a destructor; the queued native cleanup owns only the
        // retained observer and cannot touch the destroyed bridge.
        std::lock_guard lock(session->bridge_mutex);
        session->bridge = nullptr;
    }
    // Shutdown may originate on a guest worker while AppKit is waiting for
    // it. Transfer the retained observer to the main queue; never block it.
    if ([NSThread isMainThread]) {
        [session stop];
        [session release];
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{
            [session stop];
            [session release];
        });
    }
}

void CocoaBridge::publish_dock_session(bool became_key) {
    VtrDockSession* session = (VtrDockSession*)m_dock_session.load();
    if (!session) return;
    if (became_key) {
        session->active = mach_absolute_time();
        const NSEventType event = NSApp.currentEvent.type;
        if (event == NSEventTypeLeftMouseDown || event == NSEventTypeRightMouseDown ||
            event == NSEventTypeOtherMouseDown || event == NSEventTypeKeyDown)
            session->before_activation = 0;
    }
    [session publish:@"state"];
}

bool CocoaBridge::route_dock_session() {
    VtrDockSession* session = (VtrDockSession*)m_dock_session.load();
    if (!session) return false;
    if (session->routing) return false;
    [session refresh];
    if ([session owner] != getpid()) return false;
    pid_t target = [session target];
    // AppKit may key an owner's old local window before delivering its Dock
    // reopen delegate. Preserve the process that was current immediately
    // before that activation; a real local mouse/key interaction clears it.
    if (session->before_activation &&
        [NSDate timeIntervalSinceReferenceDate] - session->activation_time < 1 &&
        session->members.contains(session->before_activation) &&
        session->members.at(session->before_activation).birth == session->before_activation_birth)
        target = session->before_activation;
    session->before_activation = 0;
    if (!target) return false;
    if (target == getpid()) {
        // Local activation follows the same target rule as a remote request.
        // Do not recurse from applicationDidBecomeActive.
        session->routing = true;
        activate_dock_session_target();
        session->routing = false;
        return true;
    }
    NSRunningApplication* app = [NSRunningApplication runningApplicationWithProcessIdentifier:target];
    if (!app || app.terminated) return false;
    if (@available(macOS 14.0, *)) [NSApp yieldActivationToApplication:app];
    // Activation is asynchronous and AppKit may decline this particular
    // request. Still deliver the user's reopen to its owning process: it
    // restores the selected window and makes its own activation request.
    (void)[app activateWithOptions:0];
    [[NSDistributedNotificationCenter defaultCenter]
        postNotificationName:kSessionMessage object:session->identity
        userInfo:@{@"kind":@"activate", @"pid":@(getpid()),
                   @"birth":@(session->birth), @"target":@(target),
                   @"targetBirth":@(session->members.at(target).birth)}
        deliverImmediately:YES];
    return true;
}

void CocoaBridge::activate_dock_session_target() {
    const HWND hwnd = dock_session_target();
    const auto found = m_windows.find(hwnd);
    if (found == m_windows.end()) {
        if (m_notify_icon_reopen_target) {
            if (const auto key = m_notify_icon_reopen_target())
                report_notify_icon_input(*key, NotifyIconInput::Activate);
        }
        return;
    }
    NSWindow* window = (__bridge NSWindow*)found->second.ns_window;
    [NSApp unhide:nil];
    [NSApp activateIgnoringOtherApps:YES];
    if ([window isMiniaturized]) [window deminiaturize:nil];
    [window makeKeyAndOrderFront:nil];
    restack_owned_windows_above(hwnd);
}

std::string CocoaBridge::dock_session_group(HWND hwnd) const {
    return m_app_user_model_id_resolver ? m_app_user_model_id_resolver(hwnd) : std::string{};
}

void CocoaBridge::set_app_user_model_id_resolver(std::function<std::string(HWND)> resolver) {
    m_app_user_model_id_resolver = std::move(resolver);
}

bool CocoaBridge::dock_session_has_tray_target() const {
    return m_notify_icon_reopen_target && m_notify_icon_reopen_target().has_value();
}

void CocoaBridge::broadcast_dock_session_quit() {
    VtrDockSession* session = (VtrDockSession*)m_dock_session.load();
    if (!session) return;
    if (!session->quitting) [session publish:@"quit"];
}

} // namespace vitreo::windowing
