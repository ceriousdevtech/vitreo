// SPDX-License-Identifier: LGPL-2.1-only

/// @file system_classes.hpp
/// @brief The window classes Windows pre-registers for every process.
///
/// Static, Button, Edit, ListBox, ComboBox and ScrollBar are registered by the
/// system, not by the application, and the common controls join them once
/// comctl32 is loaded. Applications rely on being able to look them up:
/// subclassing a standard control starts with
/// `GetClassInfoW(NULL, L"Button", &wc)` to capture `wc.lpfnWndProc`, and a
/// FALSE there is fatal — WiX's ThemeInitialize fails the whole bootstrapper
/// with "Failed to initialize theme manager".
///
/// The reported wndproc must therefore be a value the caller can hand back to
/// CallWindowProc. Each native base class retains a distinct A/W procedure
/// identity so a superclass can forward creation messages to that base.
///
/// Style and cbWndExtra values are captured from a real Windows installation
/// (tests/winapi/user32/oracle_system_classes.txt).

#pragma once

#include <string_view>

#include "vitreo/types.hpp"
#include "winapi/user32/classes/window_class.hpp"

namespace vitreo::winapi {

/// A system-registered class as Windows reports it through GetClassInfo.
struct SystemClassInfo {
    std::wstring_view name;
    u32 style;
    i32 cls_extra;
    i32 wnd_extra;
    u32 hbr_background; ///< COLOR_* + 1, or 0 when the class paints its own.
    bool arrow_cursor;  ///< The class cursor is the system arrow.
};

/// Every system class, in registration order.
[[nodiscard]] const SystemClassInfo* system_classes_begin();
[[nodiscard]] const SystemClassInfo* system_classes_end();

/// Look up a system class by name, case-insensitively (window class names are
/// not case sensitive). Returns nullptr when @p name is not a system class.
[[nodiscard]] const SystemClassInfo* find_system_class(std::wstring_view name);

/// Per-base-class procedure identity, independent of an application's
/// superclass name. Registered A/W thunks are callable; pre-registration
/// identities are accepted by CallWindowProc for isolated unit tests.
[[nodiscard]] u64 system_class_wndproc(std::wstring_view name, bool ansi = false);
void set_system_class_wndproc(std::wstring_view name, bool ansi, u64 address);
[[nodiscard]] const SystemClassInfo* find_system_class_by_wndproc(u64 address);
[[nodiscard]] u64 resolve_system_class_wndproc(u64 address, bool ansi = false);

/// Fill @p out with the system class's registration, as GetClassInfo would.
/// Returns false when @p name is not a system class.
[[nodiscard]] bool fill_system_class(std::wstring_view name,
                                     WindowClassData& out);

} // namespace vitreo::winapi
