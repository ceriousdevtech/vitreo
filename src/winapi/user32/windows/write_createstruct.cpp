// SPDX-License-Identifier: LGPL-2.1-only

/// @file window.cpp
/// @brief CreateWindowExW/DestroyWindow implementation (WIN32-051)

#include "window.hpp"
#include "winapi/user32/painting/painting.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/hwnd_registry.hpp"
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <optional>

/// @file write_createstruct.cpp
/// @brief write_createstruct
///
/// Closed cut from window.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// -----------------------------------------------------------------------------
// write_createstruct
// -----------------------------------------------------------------------------

GuestAddr WindowManager::write_createstruct(
    Marshaler* m_marshaler, u32 ex_style, const std::wstring& class_name, const std::wstring& window_name,
    u32 style, i32 x, i32 y, i32 w, i32 h,
    windowing::HWND parent, u64 menu, u64 hinstance, u64 param,
    GuestAddr cls_ptr, GuestAddr name_ptr)
{
    if (!m_marshaler) return 0;

    GuestAddr cs = m_createstruct_scratch;
    if (m_marshaler->cpu_is_64bit()) {
        // CREATESTRUCTW (64-bit) layout, 80 bytes total.
        (void)m_marshaler->write_u64(cs +  0, param);
        (void)m_marshaler->write_u64(cs +  8, hinstance);
        (void)m_marshaler->write_u64(cs + 16, menu);
        (void)m_marshaler->write_u64(cs + 24, static_cast<u64>(parent));
        (void)m_marshaler->write_u32(cs + 32, static_cast<u32>(h));
        (void)m_marshaler->write_u32(cs + 36, static_cast<u32>(w));
        (void)m_marshaler->write_u32(cs + 40, static_cast<u32>(y));
        (void)m_marshaler->write_u32(cs + 44, static_cast<u32>(x));
        (void)m_marshaler->write_u32(cs + 48, style);
        (void)m_marshaler->write_u32(cs + 52, 0);
        (void)m_marshaler->write_u64(cs + 56, name_ptr);
        (void)m_marshaler->write_u64(cs + 64, cls_ptr);
        (void)m_marshaler->write_u32(cs + 72, ex_style);
        (void)m_marshaler->write_u32(cs + 76, 0);
        return cs;
    }

    // CREATESTRUCTW (32-bit) layout, 48 bytes total:
    //  +0  lpCreateParams (u32 = param truncated to 32 bits)
    //  +4  hInstance      (u32)
    //  +8  hMenu          (u32)
    // +12  hwndParent     (u32)
    // +16  cy             (i32 = h)
    // +20  cx             (i32 = w)
    // +24  y              (i32)
    // +28  x              (i32)
    // +32  style          (u32)
    // +36  lpszName       (u32 = name_ptr in guest)
    // +40  lpszClass      (u32 = cls_ptr/atom in guest)
    // +44  dwExStyle      (u32)
    (void)m_marshaler->write_u32(cs +  0, static_cast<u32>(param));
    (void)m_marshaler->write_u32(cs +  4, hinstance);
    (void)m_marshaler->write_u32(cs +  8, menu);
    (void)m_marshaler->write_u32(cs + 12, static_cast<u32>(parent));
    (void)m_marshaler->write_u32(cs + 16, static_cast<u32>(h));
    (void)m_marshaler->write_u32(cs + 20, static_cast<u32>(w));
    (void)m_marshaler->write_u32(cs + 24, static_cast<u32>(y));
    (void)m_marshaler->write_u32(cs + 28, static_cast<u32>(x));
    (void)m_marshaler->write_u32(cs + 32, style);
    (void)m_marshaler->write_u32(cs + 36, static_cast<u32>(name_ptr));
    (void)m_marshaler->write_u32(cs + 40, static_cast<u32>(cls_ptr));
    (void)m_marshaler->write_u32(cs + 44, ex_style);
    (void)class_name; // unused — guest already has the string at cls_ptr
    (void)window_name;
    return cs;
}

} // namespace vitreo::winapi
