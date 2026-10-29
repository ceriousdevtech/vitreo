// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_key_mapping.mm
/// @brief Host key codes and modifiers mapped to Win32 key messages.
///
/// macOS virtual key codes to Windows VK codes, the lParam WM_KEYDOWN and
/// WM_KEYUP carry (PC scan code, extended-key bit, transition state), and
/// the MK_* flags mouse messages carry.
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

namespace {

// True when AppKit is routing keystrokes into a native editor (NSTextField
// or its field-editor NSTextView). Guest WM_KEY* must not also be posted to
// the parent dialog. A wizard's text fields fight dual delivery and the UI
// locks up on click and type.


/// IBM PC set-1 scan code for a Windows virtual key.
///
/// The lParam scan-code field must carry a *PC* scan code, not the macOS key
/// code — they are unrelated numbering schemes ('1' is macOS 18 but PC 0x02).
/// Games from the DOS/early-Windows era frequently identify keys by scan code
/// for keyboard-layout independence, so a wrong value silently misidentifies
/// every key. Returns 0 for keys with no set-1 equivalent.
[[nodiscard]] vitreo::u8 pc_scan_code_for_vk(vitreo::u32 vk) {
    switch (vk) {
    // Digit row: '1'..'9' are contiguous, '0' follows.
    case 0x31: return 0x02; case 0x32: return 0x03; case 0x33: return 0x04;
    case 0x34: return 0x05; case 0x35: return 0x06; case 0x36: return 0x07;
    case 0x37: return 0x08; case 0x38: return 0x09; case 0x39: return 0x0A;
    case 0x30: return 0x0B;
    // Letters, in QWERTY keyboard order.
    case 'Q': return 0x10; case 'W': return 0x11; case 'E': return 0x12;
    case 'R': return 0x13; case 'T': return 0x14; case 'Y': return 0x15;
    case 'U': return 0x16; case 'I': return 0x17; case 'O': return 0x18;
    case 'P': return 0x19; case 'A': return 0x1E; case 'S': return 0x1F;
    case 'D': return 0x20; case 'F': return 0x21; case 'G': return 0x22;
    case 'H': return 0x23; case 'J': return 0x24; case 'K': return 0x25;
    case 'L': return 0x26; case 'Z': return 0x2C; case 'X': return 0x2D;
    case 'C': return 0x2E; case 'V': return 0x2F; case 'B': return 0x30;
    case 'N': return 0x31; case 'M': return 0x32;
    // Editing / control keys.
    case 0x1B: return 0x01;  // VK_ESCAPE
    case 0x08: return 0x0E;  // VK_BACK
    case 0x09: return 0x0F;  // VK_TAB
    case 0x0D: return 0x1C;  // VK_RETURN
    case 0x11: return 0x1D;  // VK_CONTROL
    case 0x10: return 0x2A;  // VK_SHIFT
    case 0x12: return 0x38;  // VK_MENU
    case 0x20: return 0x39;  // VK_SPACE
    // Punctuation.
    case 0xBA: return 0x27;  // ;
    case 0xBB: return 0x0D;  // =
    case 0xBC: return 0x33;  // ,
    case 0xBD: return 0x0C;  // -
    case 0xBE: return 0x34;  // .
    case 0xBF: return 0x35;  // /
    case 0xC0: return 0x29;  // `
    case 0xDB: return 0x1A;  // [
    case 0xDC: return 0x2B;  // backslash
    case 0xDD: return 0x1B;  // ]
    case 0xDE: return 0x28;  // '
    // Function keys.
    case 0x70: return 0x3B; case 0x71: return 0x3C; case 0x72: return 0x3D;
    case 0x73: return 0x3E; case 0x74: return 0x3F; case 0x75: return 0x40;
    case 0x76: return 0x41; case 0x77: return 0x42; case 0x78: return 0x43;
    case 0x79: return 0x44; case 0x7A: return 0x57; case 0x7B: return 0x58;
    // Arrows and navigation are extended keys sharing the numeric-pad codes.
    case 0x26: return 0x48; case 0x28: return 0x50;
    case 0x25: return 0x4B; case 0x27: return 0x4D;
    case 0x24: return 0x47; case 0x23: return 0x4F;
    case 0x21: return 0x49; case 0x22: return 0x51;
    case 0x2D: return 0x52; case 0x2E: return 0x53;
    default:   return 0;
    }
}

/// True for keys Windows marks with the extended-key bit (lParam bit 24).
[[nodiscard]] bool is_extended_vk(vitreo::u32 vk) {
    switch (vk) {
    case 0x21: case 0x22: case 0x23: case 0x24:  // PgUp/PgDn/End/Home
    case 0x25: case 0x26: case 0x27: case 0x28:  // arrows
    case 0x2D: case 0x2E:                        // Insert/Delete
        return true;
    default:
        return false;
    }
}

/// Build the lParam Win32 puts on WM_KEYDOWN/WM_KEYUP.
///
/// Layout: bits 0-15 repeat count, 16-23 OEM scan code, 24 extended key,
/// 29 context code (Alt held), 30 previous key state, 31 transition state.
/// A zero lParam claims "repeat count 0" and, on a key-up, a transition state
/// that means the key is going *down* — so applications that inspect it see
/// contradictory input.

/// Windows reports the live Shift/Ctrl state in the wParam of every mouse
/// message. Applications read it straight from the message rather than calling
/// GetKeyState. Shift-click and ctrl-click selection both read these bits
/// from the mouse message rather than calling GetKeyState.

} // namespace

// Declared in windowing/cocoa/objc_helpers.h — these must stay out of the
// anonymous namespace above, which would give them internal linkage and
// leave the event pump's calls unresolved.

bool vtr_native_text_editor_focused(NSWindow* win) {
    if (!win) return false;
    NSResponder* responder = [win firstResponder];
    return [responder isKindOfClass:[NSTextView class]] ||
           [responder isKindOfClass:[NSTextField class]] ||
           [responder isKindOfClass:[NSText class]];
}

vitreo::u64 mouse_modifier_flags(NSEvent* event) {
    const NSEventModifierFlags flags =
        event ? [event modifierFlags] : [NSEvent modifierFlags];
    vitreo::u64 mk = 0;
    if (flags & NSEventModifierFlagShift)   mk |= MK_SHIFT;
    if (flags & NSEventModifierFlagControl) mk |= MK_CONTROL;
    return mk;
}

// Shared with the cocoa_*.mm translation units through
// windowing/cocoa/objc_helpers.h, so it must have external linkage —
// it cannot live in the anonymous namespace above.
[[nodiscard]] vitreo::u64 key_message_lparam(vitreo::u32 vk,
                                             NSEventModifierFlags flags,
                                             bool key_down, bool was_down) {
    vitreo::u64 lparam = 1;  // repeat count
    lparam |= (static_cast<vitreo::u64>(pc_scan_code_for_vk(vk)) & 0xFFu) << 16;
    if (is_extended_vk(vk)) lparam |= vitreo::u64{1} << 24;
    if (flags & NSEventModifierFlagOption) lparam |= vitreo::u64{1} << 29;
    if (was_down)  lparam |= vitreo::u64{1} << 30;
    if (!key_down) lparam |= vitreo::u64{1} << 31;
    return lparam;
}

vitreo::u32 vitreo::windowing::CocoaBridge::map_native_keycode_to_windows_vk(
    vitreo::u16 key_code) noexcept {
    switch (key_code) {
    case 0x00: return 0x41; // A
    case 0x01: return 0x53; // S
    case 0x02: return 0x44; // D
    case 0x03: return 0x46; // F
    case 0x04: return 0x48; // H
    case 0x05: return 0x47; // G
    case 0x06: return 0x5A; // Z
    case 0x07: return 0x58; // X
    case 0x08: return 0x43; // C
    case 0x09: return 0x56; // V
    case 0x0B: return 0x42; // B
    case 0x0C: return 0x51; // Q
    case 0x0D: return 0x57; // W
    case 0x0E: return 0x45; // E
    case 0x0F: return 0x52; // R
    case 0x10: return 0x59; // Y
    case 0x11: return 0x54; // T
    case 0x12: return 0x31;
    case 0x13: return 0x32;
    case 0x14: return 0x33;
    case 0x15: return 0x34;
    case 0x16: return 0x36;
    case 0x17: return 0x35;
    case 0x18: return 0xBB; // VK_OEM_PLUS
    case 0x19: return 0x39;
    case 0x1A: return 0x37;
    case 0x1B: return 0xBD; // VK_OEM_MINUS
    case 0x1C: return 0x38;
    case 0x1D: return 0x30;
    case 0x1E: return 0xDD; // VK_OEM_6
    case 0x1F: return 0x4F; // O
    case 0x20: return 0x55; // U
    case 0x21: return 0xDB; // VK_OEM_4
    case 0x22: return 0x49; // I
    case 0x23: return 0x50; // P
    case 0x25: return 0x4C; // L
    case 0x26: return 0x4A; // J
    case 0x27: return 0xDE; // VK_OEM_7
    case 0x28: return 0x4B; // K
    case 0x29: return 0xBA; // VK_OEM_1
    case 0x2A: return 0xDC; // VK_OEM_5
    case 0x2B: return 0xBC; // VK_OEM_COMMA
    case 0x2C: return 0xBF; // VK_OEM_2
    case 0x2D: return 0x4E; // N
    case 0x2E: return 0x4D; // M
    case 0x2F: return 0xBE; // VK_OEM_PERIOD
    case 0x32: return 0xC0; // VK_OEM_3
    case 0x24: return 0x0D; // VK_RETURN
    case 0x30: return 0x09; // VK_TAB
    case 0x31: return 0x20; // VK_SPACE
    case 0x33: return 0x08; // VK_BACK
    case 0x35: return 0x1B; // VK_ESCAPE
    case 0x37: return 0x5B; // VK_LWIN (Command)
    case 0x38: return 0xA0; // VK_LSHIFT
    case 0x39: return 0x14; // VK_CAPITAL
    case 0x3A: return 0xA4; // VK_LMENU (Option)
    case 0x3B: return 0xA2; // VK_LCONTROL
    case 0x3C: return 0xA1; // VK_RSHIFT
    case 0x3D: return 0xA5; // VK_RMENU
    case 0x3E: return 0xA3; // VK_RCONTROL
    case 0x47: return 0x0C; // VK_CLEAR
    case 0x4B: return 0x6F; // VK_DIVIDE
    case 0x4C: return 0x0D; // Keypad Enter
    case 0x4E: return 0x6D; // VK_SUBTRACT
    case 0x51: return 0xBB; // Keypad equals
    case 0x52: return 0x60;
    case 0x53: return 0x61;
    case 0x54: return 0x62;
    case 0x55: return 0x63;
    case 0x56: return 0x64;
    case 0x57: return 0x65;
    case 0x58: return 0x66;
    case 0x59: return 0x67;
    case 0x5B: return 0x68;
    case 0x5C: return 0x69;
    case 0x41: return 0x6E; // VK_DECIMAL
    case 0x43: return 0x6A; // VK_MULTIPLY
    case 0x45: return 0x6B; // VK_ADD
    case 0x72: return 0x2D; // VK_INSERT
    case 0x73: return 0x24; // VK_HOME
    case 0x74: return 0x21; // VK_PRIOR
    case 0x75: return 0x2E; // VK_DELETE
    case 0x77: return 0x23; // VK_END
    case 0x79: return 0x22; // VK_NEXT
    case 0x7B: return 0x25; // VK_LEFT
    case 0x7C: return 0x27; // VK_RIGHT
    case 0x7D: return 0x28; // VK_DOWN
    case 0x7E: return 0x26; // VK_UP
    default:
        if (key_code >= 0x7A && key_code <= 0x7A) return 0x70; // F1
        if (key_code == 0x78) return 0x71; // F2
        if (key_code == 0x63) return 0x72; // F3
        if (key_code == 0x76) return 0x73; // F4
        if (key_code == 0x60) return 0x74; // F5
        if (key_code == 0x61) return 0x75; // F6
        if (key_code == 0x62) return 0x76; // F7
        if (key_code == 0x64) return 0x77; // F8
        if (key_code == 0x65) return 0x78; // F9
        if (key_code == 0x6D) return 0x79; // F10
        if (key_code == 0x67) return 0x7A; // F11
        if (key_code == 0x6F) return 0x7B; // F12
        return 0;
    }
}
