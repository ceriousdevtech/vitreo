#pragma once

/// @file mouse_in_pointer.hpp
/// @brief EnableMouseInPointer: hardware mouse input delivered as WM_POINTER*.
///
/// Sources: Learn EnableMouseInPointer, GetPointerInfo, POINTER_INFO,
/// WM_POINTERUPDATE; constants from the 10.0.26100.0 WinUser.h; the message
/// stream, flag words, POINTER_INFO contents and DefWindowProc promotion were
/// measured on Windows with tools/oracle_probe/mouse_in_pointer_probe.c.

#include <vitreo/common.hpp>
#include "windowing/messages/message_types.hpp"

#include <array>
#include <optional>

namespace vitreo::winapi::mouse_in_pointer {

inline constexpr u32 kMousePointerId = 1;
inline constexpr u32 PT_MOUSE = 4;

inline constexpr u32 WM_NCPOINTERUPDATE = 0x0241;
inline constexpr u32 WM_POINTERUPDATE   = 0x0245;
inline constexpr u32 WM_POINTERDOWN     = 0x0246;
inline constexpr u32 WM_POINTERUP       = 0x0247;
inline constexpr u32 WM_POINTERWHEEL    = 0x024E;
inline constexpr u32 WM_POINTERHWHEEL   = 0x024F;

inline constexpr u32 POINTER_FLAG_INRANGE      = 0x00000002;
inline constexpr u32 POINTER_FLAG_INCONTACT    = 0x00000004;
inline constexpr u32 POINTER_FLAG_FIRSTBUTTON  = 0x00000010;
inline constexpr u32 POINTER_FLAG_SECONDBUTTON = 0x00000020;
inline constexpr u32 POINTER_FLAG_THIRDBUTTON  = 0x00000040;
inline constexpr u32 POINTER_FLAG_FOURTHBUTTON = 0x00000080;
inline constexpr u32 POINTER_FLAG_FIFTHBUTTON  = 0x00000100;
inline constexpr u32 POINTER_FLAG_PRIMARY      = 0x00002000;
inline constexpr u32 POINTER_FLAG_DOWN         = 0x00010000;
inline constexpr u32 POINTER_FLAG_UPDATE       = 0x00020000;
inline constexpr u32 POINTER_FLAG_UP           = 0x00040000;
inline constexpr u32 POINTER_FLAG_WHEEL        = 0x00080000;
inline constexpr u32 POINTER_FLAG_HWHEEL       = 0x00100000;

/// POINTER_INFO in host form. Written to the guest by write_pointer_info.
struct PointerInfo {
    u32 pointer_type = 0;
    u32 pointer_id = 0;
    u32 frame_id = 0;
    u32 pointer_flags = 0;
    u64 source_device = 0;
    windowing::HWND hwnd_target = windowing::NULL_HWND;
    i32 pixel_x = 0;
    i32 pixel_y = 0;
    i32 himetric_x = 0;
    i32 himetric_y = 0;
    u32 time = 0;
    u32 history_count = 0;
    i32 input_data = 0;
    u32 key_states = 0;
    u64 performance_count = 0;
    u32 button_change_type = 0;
};

struct EnableResult {
    bool ok = false;
    u32 error = 0;
};

/// EnableMouseInPointer(fEnable): the first call in the process sets the
/// state; later calls succeed only when they ask for the current state and
/// otherwise fail with ERROR_ACCESS_DENIED. A value other than FALSE/TRUE
/// fails with ERROR_INVALID_PARAMETER.
EnableResult enable(u64 f_enable) noexcept;
[[nodiscard]] bool enabled() noexcept;

/// Rewrite a retrieved hardware client-area mouse message (WM_MOUSEMOVE,
/// button, wheel) as its WM_POINTER* equivalent when mouse-in-pointer is
/// enabled, and record the POINTER_INFO for @p thread_id. Returns false and
/// leaves @p msg unchanged otherwise.
bool convert(windowing::MSG& msg, u32 thread_id, u64 performance_count) noexcept;

/// GetPointerInfo: 0 on success or the Win32 error to report.
u32 pointer_info(u32 thread_id, u32 pointer_id, PointerInfo& out) noexcept;

/// GetPointerType: 0 on success or the Win32 error to report.
u32 pointer_type(u32 pointer_id, u32& out) noexcept;

/// DefWindowProc for a pointer message converted by convert(): the mouse
/// message it was generated from, to be queued as promoted hardware input.
std::optional<windowing::MSG> promote(u32 thread_id, windowing::HWND hwnd,
                                      u32 message, u64 w_param,
                                      u64 l_param) noexcept;

/// POINTER_INFO guest image: 88 bytes on x86, 96 on x64 (WinUser.h layout,
/// sizes and offsets measured on Windows for both bitnesses).
struct PointerInfoImage {
    std::array<u8, 96> bytes{};
    usize size = 0;
};
[[nodiscard]] PointerInfoImage pointer_info_image(const PointerInfo& info,
                                                  bool is_64bit) noexcept;

void reset_for_testing() noexcept;

}  // namespace vitreo::winapi::mouse_in_pointer
