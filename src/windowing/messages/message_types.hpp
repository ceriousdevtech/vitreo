#pragma once
/// @file message_types.hpp
/// @brief Windows message types and constants (WINDOW-002)

#include "windowing/model/window_types.hpp"

namespace vitreo::windowing {

// Windows message identifiers
namespace WM {
    constexpr u32 null_msg    = 0x0000;  // WM_NULL (avoid name conflict with NULL macro)
    constexpr u32 CREATE      = 0x0001;
    constexpr u32 DESTROY     = 0x0002;
    constexpr u32 MOVE        = 0x0003;
    constexpr u32 SIZE        = 0x0005;
    constexpr u32 ACTIVATE    = 0x0006;
    constexpr u32 SETFOCUS    = 0x0007;
    constexpr u32 KILLFOCUS   = 0x0008;
    constexpr u32 ENABLE      = 0x000A;
    constexpr u32 SETTEXT     = 0x000C;
    constexpr u32 GETTEXT     = 0x000D;
    constexpr u32 GETTEXTLENGTH = 0x000E;
    constexpr u32 PAINT       = 0x000F;
    constexpr u32 CLOSE       = 0x0010;
    constexpr u32 QUIT        = 0x0012;
    constexpr u32 ERASEBKGND  = 0x0014;
    constexpr u32 SHOWWINDOW    = 0x0018;
    constexpr u32 ACTIVATEAPP  = 0x001C;
    constexpr u32 WINDOWPOSCHANGING = 0x0046;
    constexpr u32 WINDOWPOSCHANGED  = 0x0047;
    constexpr u32 NOTIFY       = 0x004E;
    constexpr u32 NCCREATE    = 0x0081;
    constexpr u32 NCDESTROY   = 0x0082;
    constexpr u32 NCCALCSIZE  = 0x0083;
    constexpr u32 NCACTIVATE  = 0x0086;
    constexpr u32 NCLBUTTONDOWN = 0x00A1;
    constexpr u32 KEYDOWN     = 0x0100;
    constexpr u32 KEYUP       = 0x0101;
    constexpr u32 CHAR_MSG    = 0x0102;  // WM_CHAR (avoid name conflict)
    constexpr u32 SYSKEYDOWN  = 0x0104;
    constexpr u32 SYSKEYUP    = 0x0105;
    constexpr u32 SYSCHAR     = 0x0106;
    constexpr u32 COMMAND     = 0x0111;
    constexpr u32 TIMER       = 0x0113;
    constexpr u32 INPUT       = 0x00FF;
    constexpr u32 MOUSEMOVE   = 0x0200;
    constexpr u32 LBUTTONDOWN = 0x0201;
    constexpr u32 LBUTTONUP   = 0x0202;
    constexpr u32 LBUTTONDBLCLK = 0x0203;
    constexpr u32 RBUTTONDOWN = 0x0204;
    constexpr u32 RBUTTONUP   = 0x0205;
    constexpr u32 RBUTTONDBLCLK = 0x0206;
    constexpr u32 MBUTTONDOWN = 0x0207;
    constexpr u32 MBUTTONUP   = 0x0208;
    constexpr u32 MOUSEWHEEL  = 0x020A;
    // Delivered only after TrackMouseEvent arms them, and each request is
    // cancelled by its own delivery (Learn: TrackMouseEvent).
    constexpr u32 MOUSEHOVER  = 0x02A1;
    constexpr u32 MOUSELEAVE  = 0x02A3;
    constexpr u32 MOUSEHWHEEL = 0x020E;
    constexpr u32 DRAWCLIPBOARD = 0x0308;
    constexpr u32 CHANGECBCHAIN = 0x030D;
    constexpr u32 NCHITTEST   = 0x0084;
    constexpr u32 SETCURSOR   = 0x0020;
    constexpr u32 MOUSEACTIVATE = 0x0021;
    constexpr u32 GETMINMAXINFO = 0x0024;
    constexpr u32 QUERYOPEN   = 0x0013;
    constexpr u32 SYSCOMMAND  = 0x0112;
    constexpr u32 INITDIALOG  = 0x0110;
}

// WM_ACTIVATE wParam low word (winuser.h).
namespace WA {
    constexpr u32 INACTIVE    = 0;
    constexpr u32 ACTIVE      = 1;
    constexpr u32 CLICKACTIVE = 2;
}

// Peek message remove flags
namespace PM {
    constexpr u32 NOREMOVE = 0x0000;
    constexpr u32 REMOVE   = 0x0001;
    constexpr u32 NOYIELD  = 0x0002;
    /// PM_QS_* (winuser.h): the high word of wRemoveMsg is a QS_* mask
    /// restricting which message classes PeekMessage processes; 0 = all.
    constexpr u32 QS_SHIFT = 16;
}

// GetQueueStatus / MsgWait wake-mask bits (winuser.h).
namespace QS {
    constexpr u32 KEY            = 0x0001;
    constexpr u32 MOUSEMOVE      = 0x0002;
    constexpr u32 MOUSEBUTTON    = 0x0004;
    constexpr u32 POSTMESSAGE    = 0x0008;
    constexpr u32 TIMER          = 0x0010;
    constexpr u32 PAINT          = 0x0020;
    constexpr u32 SENDMESSAGE    = 0x0040;
    constexpr u32 HOTKEY         = 0x0080;
    constexpr u32 ALLPOSTMESSAGE = 0x0100;
    constexpr u32 RAWINPUT       = 0x0400;
    constexpr u32 TOUCH          = 0x0800;
    constexpr u32 POINTER        = 0x1000;
    constexpr u32 MOUSE          = MOUSEMOVE | MOUSEBUTTON;
    constexpr u32 INPUT =
        MOUSE | KEY | RAWINPUT | TOUCH | POINTER;
    constexpr u32 ALLEVENTS =
        INPUT | POSTMESSAGE | TIMER | PAINT | HOTKEY;
    constexpr u32 ALLINPUT =
        INPUT | POSTMESSAGE | TIMER | PAINT | HOTKEY | SENDMESSAGE;
    /// Bits GetQueueStatus accepts. Measured on Windows 11 by
    /// tools/oracle_probe/s2msg_queue_probe (status_bit_*_rejected): 0x200,
    /// 0x2000 and 0x8000 upward fail with ERROR_INVALID_FLAGS; 0x4000 is an
    /// accepted undocumented bit.
    constexpr u32 VALID_STATUS_FLAGS = 0x5DFFu;
}

// MSG structure
struct MSG {
    HWND   hwnd    = NULL_HWND;
    u32    message = 0;
    u64    wParam  = 0;
    u64    lParam  = 0;
    u32    time    = 0;
    Point  pt      = {};
    /// Internal routing metadata for hwnd-less thread messages. This field is
    /// not part of the guest-visible Win32 MSG layout and is never marshaled.
    u32    target_thread_id = 0;
    /// Host CLOCK_MONOTONIC nanoseconds at which this message entered the
    /// queue, stamped by MessageQueue::post_impl. Diagnostic only (see
    /// VITREO_INPUT_LATENCY): not guest-visible and never marshaled, so it does
    /// NOT survive a round trip through guest memory — DispatchMessage reads
    /// the MSG back from the guest, which is why input_latency.hpp carries the
    /// peek-side timestamp in a thread-local instead.
    u64    host_post_ns = 0;
    /// Internal, never marshaled: the message came from the hardware input
    /// stream (MessageQueue::post_input) rather than PostMessage.
    bool   hardware_input = false;
    /// Internal, never marshaled: the dwExtraInfo of the input event this
    /// hardware message came from (KEYBDINPUT / MOUSEINPUT); 0 otherwise.
    /// Retrieval makes it the thread's GetMessageExtraInfo value.
    u64    extra_info = 0;
    /// Internal, never marshaled: the UTF-16 unit a VK_PACKET keystroke
    /// (SendInput KEYEVENTF_UNICODE) types; TranslateMessage reads it.
    u16    packet_char = 0;
    /// Internal, never marshaled: a mouse message DefWindowProc generated from
    /// an unhandled WM_POINTER* message; it is not converted back to pointer
    /// input.
    bool   pointer_promoted = false;
};

} // namespace vitreo::windowing
