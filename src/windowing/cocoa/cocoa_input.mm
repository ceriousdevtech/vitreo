// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_input.mm
/// @brief Input state: keyboard, mouse, cursor position and clipping.
///
/// Windows exposes input state as something a guest can *poll* — GetAsyncKeyState, GetCursorPos, ClipCursor — not only as messages it receives. macOS answers those through NSEvent and CGEvent, and reconciling the two models is what this file does.
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

// =============================================================================
// Native Delegation: Input State (NSEvent)
// =============================================================================

i16 CocoaBridge::get_native_key_state(u32 vk_code) const {
    if (vk_code < 256) {
        const u64 mask = u64{1} << (vk_code & 63u);
        const bool down = (m_native_keys_down[vk_code >> 6u].load(
            std::memory_order_relaxed) & mask) != 0;
        if (down) return static_cast<i16>(0x8000);
    }
    @autoreleasepool {
        NSUInteger flags = [NSEvent modifierFlags];

        // Map common Windows VK codes to macOS modifier flags
        switch (vk_code) {
            case 0x10: // VK_SHIFT
                return (flags & NSEventModifierFlagShift) ? static_cast<i16>(0x8000) : 0;
            case 0x11: // VK_CONTROL
                return (flags & NSEventModifierFlagControl) ? static_cast<i16>(0x8000) : 0;
            case 0x12: // VK_MENU (Alt)
                return (flags & NSEventModifierFlagOption) ? static_cast<i16>(0x8000) : 0;
            case 0x14: // VK_CAPITAL (Caps Lock)
                return (flags & NSEventModifierFlagCapsLock) ? static_cast<i16>(0x0001) : 0;
            case 0x01: // VK_LBUTTON
                return ([NSEvent pressedMouseButtons] & 1) ? static_cast<i16>(0x8000) : 0;
            case 0x02: // VK_RBUTTON
                return ([NSEvent pressedMouseButtons] & 2) ? static_cast<i16>(0x8000) : 0;
            default:
                return 0;
        }
    }
}

bool CocoaBridge::native_key_is_down(u32 vk_code) const noexcept {
    if (vk_code >= 256) return false;
    const u64 bit = u64{1} << (vk_code & 63u);
    return (m_native_keys_down[vk_code >> 6u].load(std::memory_order_relaxed) & bit) != 0;
}

void CocoaBridge::post_modifier_transitions(HWND target, u64 modifier_flags,
                                            MessageQueue& queue) {
    // VK_SHIFT / VK_CONTROL / VK_MENU. Windows also delivers the left/right
    // specific keys, but the generic ones are what applications test.
    struct ModifierKey {
        u64 mask;
        u32 vk;
    };
    static constexpr ModifierKey kModifiers[] = {
        {static_cast<u64>(NSEventModifierFlagShift),   0x10},
        {static_cast<u64>(NSEventModifierFlagControl), 0x11},
        {static_cast<u64>(NSEventModifierFlagOption),  0x12},
    };

    for (const auto& mod : kModifiers) {
        const bool down = (modifier_flags & mod.mask) != 0;
        const bool was_down = native_key_is_down(mod.vk);
        if (down == was_down) continue;

        note_native_key_transition(mod.vk, down);
        MSG msg{};
        msg.hwnd    = target;
        msg.message = down ? WM::KEYDOWN : WM::KEYUP;
        msg.wParam  = mod.vk;
        msg.lParam  = key_message_lparam(mod.vk, modifier_flags, down, was_down);
        msg.time    = static_cast<u32>(continuous_tick_count_ms());
        queue.post_input(msg);
        if (diag::input_diag()) {
            std::fprintf(stderr, "[input] modifier vk=0x%02x %s (mask=0x%llx)\n",
                         mod.vk, down ? "DOWN" : "UP",
                         static_cast<unsigned long long>(modifier_flags));
        }
    }
}

void CocoaBridge::note_native_key_transition(u32 vk_code, bool down) noexcept {
    if (vk_code >= 256) return;
    const usize word = vk_code >> 6u;
    const u64 mask = u64{1} << (vk_code & 63u);
    if (down) {
        m_native_keys_down[word].fetch_or(mask, std::memory_order_relaxed);
        m_async_key_presses[word].fetch_or(mask, std::memory_order_relaxed);
    } else {
        m_native_keys_down[word].fetch_and(~mask, std::memory_order_relaxed);
    }
    if (diag::input_diag()) {
        std::fprintf(stderr, "[input] native key %s vk=0x%02x\n",
                     down ? "down" : "up", vk_code);
    }
    if (m_native_key_transition_callback) {
        m_native_key_transition_callback(vk_code, down);
    }
}

void CocoaBridge::note_native_mouse_button_transition(u32 vk_code,
                                                       bool down,
                                                       HWND hwnd) noexcept {
    const u32 state_bit = vk_code == 0x01 ? 1u : vk_code == 0x02 ? 2u : 0u;
    if (down && state_bit != 0) {
        m_async_mouse_presses.fetch_or(state_bit, std::memory_order_relaxed);
        if (diag::input_diag()) {
            std::fprintf(stderr, "[input] native mouse down vk=0x%02x\n",
                         vk_code);
        }
    }
    const u32 button_index = vk_code == 0x01 ? 0u : vk_code == 0x02 ? 1u :
                             vk_code == 0x04 ? 2u : UINT32_MAX;
    if (button_index != UINT32_MAX && m_native_mouse_button_callback) {
        m_native_mouse_button_callback(button_index, down, hwnd);
    }
}

void CocoaBridge::note_native_mouse_motion(i32 screen_x, i32 screen_y,
                                            double device_dx,
                                            double device_dy,
                                            HWND hwnd) noexcept {
    if (m_native_mouse_motion_callback) {
        m_native_mouse_motion_callback(screen_x, screen_y, hwnd, device_dx,
                                       device_dy);
    }
}

void CocoaBridge::note_native_mouse_motion(i32 screen_x, i32 screen_y,
                                            void* ns_event,
                                            HWND hwnd) noexcept {
    double device_dx = 0;
    double device_dy = 0;
    native_mouse_device_delta(ns_event, device_dx, device_dy);
    note_native_mouse_motion(screen_x, screen_y, device_dx, device_dy, hwnd);
}

void CocoaBridge::native_mouse_device_delta(void* ns_event, double& device_dx,
                                            double& device_dy) noexcept {
    NSEvent* event = (__bridge NSEvent*)ns_event;
    device_dx = [event deltaX];
    device_dy = [event deltaY];
    m_cursor_warp_deltas.remove_warps([event timestamp], device_dx, device_dy);
}

void CocoaBridge::note_offwindow_mouse_motion(void* ns_event) noexcept {
    // Moving the cursor onto another monitor does not stop a full-screen
    // game's mouse look on Windows (offwindow_motion_reaches_guest).
    if (!offwindow_motion_reaches_guest([NSApp isActive] == YES,
                                        !m_windows.empty())) {
        return;
    }
    double device_dx = 0;
    double device_dy = 0;
    native_mouse_device_delta(ns_event, device_dx, device_dy);
    if (device_dx == 0 && device_dy == 0) return;
    i32 screen_x = 0;
    i32 screen_y = 0;
    get_native_cursor_pos(screen_x, screen_y);
    // No event window: raw input falls back to the active window.
    note_native_mouse_motion(screen_x, screen_y, device_dx, device_dy,
                             NULL_HWND);
}

void CocoaBridge::note_host_cursor_warp(double time, double dx,
                                        double dy) noexcept {
    m_cursor_warp_deltas.note_warp(time, dx, dy);
}

bool CocoaBridge::warp_host_cursor(double x, double y) {
    // Where the cursor is now, in the same CG global space as the target.
    CGPoint from = CGPointZero;
    if (CGEventRef now = CGEventCreate(nullptr)) {
        from = CGEventGetLocation(now);
        CFRelease(now);
    }
    const double time = [[NSProcessInfo processInfo] systemUptime];
    if (CGWarpMouseCursorPosition(CGPointMake(static_cast<CGFloat>(x),
                                              static_cast<CGFloat>(y))) !=
        kCGErrorSuccess) {
        return false;
    }
    // Without this the OS keeps applying the pre-warp delta for a moment and
    // the next reported position undoes the warp.
    CGAssociateMouseAndMouseCursorPosition(true);
    note_host_cursor_warp(time, x - from.x, y - from.y);
    return true;
}

u32 CocoaBridge::last_input_tick_count() const noexcept {
    const u64 now_ms = continuous_tick_count_ms();
    const CFTimeInterval idle_seconds =
        CGEventSourceSecondsSinceLastEventType(
            kCGEventSourceStateCombinedSessionState,
            kCGAnyInputEventType);

    if (!std::isfinite(idle_seconds) || idle_seconds < 0.0) {
        return static_cast<u32>(now_ms);
    }

    const long double idle_ms_value =
        static_cast<long double>(idle_seconds) * 1000.0L;
    const u64 idle_ms = idle_ms_value >= static_cast<long double>(now_ms)
        ? now_ms
        : static_cast<u64>(idle_ms_value);
    return static_cast<u32>(now_ms - idle_ms);
}

i16 CocoaBridge::get_native_async_key_state(u32 vk_code) {
    i16 state = get_native_key_state(vk_code);
    if (vk_code < 256) {
        const usize word = vk_code >> 6u;
        const u64 mask = u64{1} << (vk_code & 63u);
        const u64 pending = m_async_key_presses[word].fetch_and(
            ~mask, std::memory_order_relaxed);
        if ((pending & mask) != 0) {
            state = static_cast<i16>(static_cast<u16>(state) | 0x0001u);
        }
    }
    const u32 button = vk_code == 0x01 ? 1u : vk_code == 0x02 ? 2u : 0u;
    if (button == 0) return state;

    const u32 pending = m_async_mouse_presses.fetch_and(
        ~button, std::memory_order_relaxed);
    if ((pending & button) != 0) {
        state = static_cast<i16>(static_cast<u16>(state) | 0x0001u);
    }
    return state;
}

void CocoaBridge::get_native_cursor_pos(i32& out_x, i32& out_y) const {
    @autoreleasepool {
        NSPoint loc = [NSEvent mouseLocation];
        const HWND hwnd = active_hwnd();
        auto active = m_windows.find(hwnd);
        if (active != m_windows.end() && active->second.ns_window) {
            NSWindow* window = reinterpret_cast<NSWindow*>(active->second.ns_window);
            NSPoint window_point = [window convertPointFromScreen:loc];
            i32 guest_x = 0;
            i32 guest_y = 0;
            // Exclusive full-screen must always clamp letterbox samples. A host
            // fallback (raw NSEvent coordinates) reports Y values like 800+ on
            // a 640x480 guest. The title then treats the left pillar as both
            // "x==0" and "y>=bottom" and the camera walks into the map corner.
            //
            // Windowed GetCursorPos has the same duty while the cursor is over
            // the content: WM_MOUSEMOVE is remapped through the drawable in
            // retarget_presentation_mouse, but polling used to return raw view
            // points. After D3D9 Reset to a listed pixel mode the view stays
            // in Cocoa points and the backbuffer is the chosen mode, so the
            // cursor misses every hit-test that uses GetCursorPos.
            const bool covers_screen = active->second.fullscreen;
            NSView* content = [window contentView];
            const NSPoint content_point =
                [content convertPoint:window_point fromView:nil];
            const bool in_content =
                content != nil &&
                NSPointInRect(content_point, [content bounds]);
            const bool map_presentation = native_cursor_pos_maps_presentation(
                covers_screen, in_content);
            if (map_native_point_to_guest_surface(window, window_point,
                                                  map_presentation, guest_x,
                                                  guest_y)) {
                // A full-screen window covers the entire guest display, so its
                // client space *is* screen space. Its bookkeeping rect can
                // still carry the pre-full-screen origin (the guest positions
                // its window inside the mode it selected), and adding that
                // origin here would report a cursor that never moves.
                out_x = (covers_screen ? 0 : active->second.frame.left) + guest_x;
                out_y = (covers_screen ? 0 : active->second.frame.top) + guest_y;
                if (diag::input_diag()) {
                    std::fprintf(stderr,
                                 "[input] cursor hwnd=0x%x screen=(%d,%d) client=(%d,%d) fs=%d\n",
                                 hwnd, out_x, out_y, guest_x, guest_y,
                                 covers_screen ? 1 : 0);
                }
                return;
            }
            if (covers_screen) {
                // Presentation surface not ready yet; still must not leak host
                // coordinates into a logical 640x480 (etc.) mode.
                out_x = 0;
                out_y = 0;
                return;
            }
        }

        NSScreen* screen = [NSScreen mainScreen];
        CGFloat screen_height = screen ? [screen frame].size.height : 1080.0;

        // Convert Cocoa Y-up to Windows Y-down
        out_x = static_cast<i32>(loc.x);
        out_y = static_cast<i32>(screen_height - loc.y);
    }
}

bool CocoaBridge::set_native_cursor_pos(i32 x, i32 y) {
    __block bool moved = false;
    vtr_run_on_main_sync(^{
        @autoreleasepool {
            // Translate the guest screen position back through the same
            // window/viewport mapping get_native_cursor_pos reads it through.
            double screen_x = x;
            double screen_y = y;
            bool mapped = false;

            const HWND hwnd = active_hwnd();
            double cocoa_x = 0;
            double cocoa_y = 0;
            if (map_guest_screen_to_cocoa(hwnd, x, y, cocoa_x, cocoa_y)) {
                // CGWarpMouseCursorPosition wants CG global coordinates, which
                // flip against the primary screen (cocoa_virtual_top_y), not
                // the screen the window is on: a window on a display of
                // another height or vertical offset warped to the wrong row.
                screen_x = cocoa_x;
                screen_y = static_cast<double>(cocoa_virtual_top_y()) - cocoa_y;
                mapped = true;
            }

            moved = warp_host_cursor(screen_x, screen_y);
            if (diag::input_diag()) {
                std::fprintf(stderr,
                             "[input] SetCursorPos guest=(%d,%d) -> screen=(%.1f,%.1f) mapped=%d ok=%d\n",
                             x, y, screen_x, screen_y, mapped ? 1 : 0,
                             moved ? 1 : 0);
            }
        }
    });
    return moved;
}

bool CocoaBridge::map_guest_screen_to_cocoa(HWND hwnd, i32 guest_x, i32 guest_y,
                                            double& cocoa_x,
                                            double& cocoa_y,
                                            bool require_surface) const {
    auto active = m_windows.find(hwnd);
    if (active == m_windows.end() || !active->second.ns_window) return false;

    NSWindow* window =
        reinterpret_cast<NSWindow*>(active->second.ns_window);
    // Mirror GetCursorPos: full-screen client space is screen space, so the
    // window origin is not subtracted.
    const bool covers_screen = active->second.fullscreen;
    const i32 client_x =
        guest_x - (covers_screen ? 0 : active->second.frame.left);
    const i32 client_y =
        guest_y - (covers_screen ? 0 : active->second.frame.top);
    NSPoint window_point{};
    if (!map_guest_surface_point_to_native(window, client_x, client_y,
                                           window_point, require_surface)) {
        return false;
    }
    const NSPoint screen_point = [window convertPointToScreen:window_point];
    cocoa_x = screen_point.x;
    cocoa_y = screen_point.y;
    return true;
}

void CocoaBridge::set_cursor_clip(bool enabled, i32 left, i32 top, i32 right,
                                  i32 bottom) {
    vtr_run_on_main_sync(^{
        if (!enabled || right <= left || bottom <= top) {
            m_guest_cursor_clip_enabled = false;
            m_guest_cursor_clip_win = {};
        } else {
            m_guest_cursor_clip_enabled = true;
            m_guest_cursor_clip_win = Rect{left, top, right, bottom};
        }
        refresh_cursor_clip_state();
        enforce_cursor_clip();
        VITREO_TRACE("[cocoa] ClipCursor enabled=%d win=(%d,%d)-(%d,%d)\n",
                     m_guest_cursor_clip_enabled ? 1 : 0, left, top, right,
                     bottom);
    });
}

bool CocoaBridge::get_cursor_clip(i32& left, i32& top, i32& right,
                                  i32& bottom) const {
    if (!m_guest_cursor_clip_enabled) return false;
    left = m_guest_cursor_clip_win.left;
    top = m_guest_cursor_clip_win.top;
    right = m_guest_cursor_clip_win.right;
    bottom = m_guest_cursor_clip_win.bottom;
    return true;
}

void CocoaBridge::refresh_cursor_clip_state() {
    // Main-thread only (callers use vtr_run_on_main_sync or are already on it).
    @autoreleasepool {
        bool want = false;
        bool surface_pending = false;
        double x_min = 0, y_min = 0, x_max = 0, y_max = 0;
        const CGFloat top_y = cocoa_virtual_top_y();

        const bool exclusive_fs =
            m_exclusive_cursor_confine_hwnd != NULL_HWND &&
            !m_fullscreen_presentation_suspended && [NSApp isActive];
        HWND exclusive_hwnd = NULL_HWND;
        bool exclusive_is_fullscreen = false;
        if (exclusive_fs) {
            auto it = m_windows.find(m_exclusive_cursor_confine_hwnd);
            // Only an EXCLUSIVE display mode (ChangeDisplaySettings / DXGI
            // exclusive) owns the display and confines the cursor by itself.
            // A display-sized borderless window (windowed fullscreen) is
            // hosted in the same Space, but on Windows its cursor roams every
            // monitor until the guest calls ClipCursor -- which is the
            // guest-clip path below, not this implicit one.
            if (it != m_windows.end() && it->second.fullscreen &&
                !it->second.borderless_fullscreen) {
                exclusive_hwnd = m_exclusive_cursor_confine_hwnd;
                exclusive_is_fullscreen = true;
            }
        }

        // Learn (ClipCursor): the cursor is a shared resource — a window may
        // confine it only while it is active, and the confinement must not
        // outlive that. Windows drops the clip when the owning application
        // stops being foreground, which is why guests re-apply ClipCursor on
        // WM_ACTIVATE. Minimising must therefore free the host cursor even
        // though the guest still believes its clip is installed: keep the
        // rectangle (GetClipCursor still reports it) but stop enforcing it.
        bool any_usable_window = false;
        for (const auto& [clip_hwnd, clip_state] : m_windows) {
            (void)clip_hwnd;
            if (clip_state.ns_window == nullptr) continue;
            auto* clip_win = reinterpret_cast<NSWindow*>(clip_state.ns_window);
            if ([clip_win isVisible] && ![clip_win isMiniaturized]) {
                any_usable_window = true;
                break;
            }
        }
        const bool guest_clip_active = guest_clip_should_apply(
            m_guest_cursor_clip_enabled, [NSApp isActive] == YES,
            any_usable_window);

        if (guest_clip_active) {
            const Rect& w = m_guest_cursor_clip_win;
            // ClipCursor uses the same screen space as GetCursorPos. In
            // exclusive fullscreen that space is the guest logical mode
            // mapped through the aspect-fit presentation — not 1:1 host
            // desktop pixels (which would lock the cursor to a 640×480-style
            // box on the display).
            if (exclusive_is_fullscreen) {
                double c0x = 0, c0y = 0, c1x = 0, c1y = 0;
                double c2x = 0, c2y = 0, c3x = 0, c3y = 0;
                const i32 x1 = w.right - 1;
                const i32 y1 = w.bottom - 1;
                if (map_guest_screen_to_cocoa(exclusive_hwnd, w.left, w.top,
                                              c0x, c0y, true) &&
                    map_guest_screen_to_cocoa(exclusive_hwnd, x1, w.top, c1x,
                                              c1y, true) &&
                    map_guest_screen_to_cocoa(exclusive_hwnd, w.left, y1, c2x,
                                              c2y, true) &&
                    map_guest_screen_to_cocoa(exclusive_hwnd, x1, y1, c3x,
                                              c3y, true)) {
                    x_min = std::min(std::min(c0x, c1x), std::min(c2x, c3x));
                    x_max = std::max(std::max(c0x, c1x), std::max(c2x, c3x));
                    y_min = std::min(std::min(c0y, c1y), std::min(c2y, c3y));
                    y_max = std::max(std::max(c0y, c1y), std::max(c2y, c3y));
                    want = x_max > x_min && y_max > y_min;
                }
                if (!want) {
                    // Presentation surface not ready yet — confine to the
                    // exclusive display so the cursor cannot escape mid-mode
                    // switch, and map the guest rectangle once it has
                    // presented (enforce_cursor_clip retries).
                    const Rect& f =
                        m_windows.find(exclusive_hwnd)->second.fullscreen_screen_frame;
                    x_min = static_cast<double>(f.left);
                    y_min = static_cast<double>(f.top);
                    x_max = static_cast<double>(f.right);
                    y_max = static_cast<double>(f.bottom);
                    want = x_max > x_min && y_max > y_min;
                    surface_pending = true;
                }
            } else {
                x_min = static_cast<double>(w.left);
                x_max = static_cast<double>(w.right);
                y_min = static_cast<double>(top_y) - static_cast<double>(w.bottom);
                y_max = static_cast<double>(top_y) - static_cast<double>(w.top);
                want = x_max > x_min && y_max > y_min;
            }
        } else if (exclusive_is_fullscreen) {
            // fullscreen_screen_frame is an NSScreen frame in Cocoa coords
            // (left/top = origin, right/bottom = origin+size).
            const Rect& f =
                m_windows.find(exclusive_hwnd)->second.fullscreen_screen_frame;
            x_min = static_cast<double>(f.left);
            y_min = static_cast<double>(f.top);
            x_max = static_cast<double>(f.right);
            y_max = static_cast<double>(f.bottom);
            want = x_max > x_min && y_max > y_min;
        }

        m_cursor_clip_cocoa_active = want;
        m_cursor_clip_surface_pending = surface_pending;
        if (diag::input_diag()) {
            std::fprintf(stderr,
                         "[input] cursor-clip guest=%d (%d,%d)-(%d,%d) exclusive=%d "
                         "cocoa=%d (%.1f,%.1f)-(%.1f,%.1f) surface_pending=%d\n",
                         guest_clip_active ? 1 : 0, m_guest_cursor_clip_win.left,
                         m_guest_cursor_clip_win.top, m_guest_cursor_clip_win.right,
                         m_guest_cursor_clip_win.bottom,
                         exclusive_is_fullscreen ? 1 : 0, want ? 1 : 0, x_min,
                         y_min, x_max, y_max, surface_pending ? 1 : 0);
        }
        m_clip_cocoa_x_min = x_min;
        m_clip_cocoa_y_min = y_min;
        m_clip_cocoa_x_max = x_max;
        m_clip_cocoa_y_max = y_max;

        if (want && !m_cursor_clip_monitor) {
            CocoaBridge* self = this;
            const NSEventMask mask =
                (NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged |
                 NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged);
            id local = [NSEvent addLocalMonitorForEventsMatchingMask:mask
                                                             handler:^NSEvent*(
                                                                 NSEvent* event) {
                                                                 self->enforce_cursor_clip();
                                                                 return event;
                                                             }];
            m_cursor_clip_monitor = (__bridge_retained void*)local;
            // Global monitor catches the cursor as it crosses onto another
            // display (local monitors only see events for this app).
            id global =
                [NSEvent addGlobalMonitorForEventsMatchingMask:mask
                                                       handler:^(NSEvent*) {
                                                           self->enforce_cursor_clip();
                                                       }];
            m_cursor_clip_global_monitor = (__bridge_retained void*)global;
        } else if (!want && m_cursor_clip_monitor) {
            id local = (__bridge_transfer id)m_cursor_clip_monitor;
            m_cursor_clip_monitor = nullptr;
            [NSEvent removeMonitor:local];
            if (m_cursor_clip_global_monitor) {
                id global = (__bridge_transfer id)m_cursor_clip_global_monitor;
                m_cursor_clip_global_monitor = nullptr;
                [NSEvent removeMonitor:global];
            }
        }
    }
}

void CocoaBridge::enforce_cursor_clip() {
    // A clip held at the whole display while the surface was not ready is
    // remapped as soon as the surface can map it (first present).
    if (m_cursor_clip_surface_pending) refresh_cursor_clip_state();
    if (!m_cursor_clip_cocoa_active) return;
    @autoreleasepool {
        NSPoint loc = [NSEvent mouseLocation];
        const double margin = 1.0;
        const double max_x = m_clip_cocoa_x_max - margin;
        const double max_y = m_clip_cocoa_y_max - margin;
        const double min_x = m_clip_cocoa_x_min + margin;
        const double min_y = m_clip_cocoa_y_min + margin;
        if (max_x <= min_x || max_y <= min_y) return;

        double x = loc.x;
        double y = loc.y;
        bool outside = false;
        if (x < min_x) { x = min_x; outside = true; }
        if (x > max_x) { x = max_x; outside = true; }
        if (y < min_y) { y = min_y; outside = true; }
        if (y > max_y) { y = max_y; outside = true; }
        if (!outside) return;

        // CGWarpMouseCursorPosition uses top-left global coordinates on the
        // primary-display space; convert from Cocoa bottom-left using the
        // same virtual top as monitor enumeration.
        const CGFloat top_y = cocoa_virtual_top_y();
        (void)warp_host_cursor(x, static_cast<double>(top_y) - y);
    }
}
} // namespace vitreo::windowing
