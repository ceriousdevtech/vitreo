// SPDX-License-Identifier: LGPL-2.1-only

/// @file exports_core.cpp
/// @brief user32 extended export table, slice "core".
///
/// Menus, window enumeration, system metrics and resource loading.
///
/// One contiguous run of the former create_user32_ext_thunks body, moved
/// verbatim; user32_ext_registration.cpp calls the slices in registration
/// order.

#include "winapi/user32/registration/registration_parts.hpp"

#include "ipc/hwnd_registry.hpp"
#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/windows/nc_metrics.hpp"
#include "winapi/user32/windows/set_cursor_pos.hpp"
#include "winapi/user32/messages/mouse_in_pointer.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/user32/windows/kbd_layout.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "winapi/user32/windows/window.hpp"
#include "winapi/user32/classes/window_class.hpp"
#include "winapi/user32/resources/resources.hpp"
#include "windowing/resources/resource_walker.hpp"
#include "core/pe/parser/pe_structures.hpp"
#include "vitreo/result.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <set>

#include <mutex>

#include <unordered_map>

namespace vitreo::winapi {

void add_user32_ext_exports_core(DllExports& exports, User32ExtManager& mgr) {
    exports.entries.push_back(ExportEntry{
        "AppendMenuA", 0,
        [&mgr](Marshaler& m) { mgr.impl_AppendMenuA(m); },
        16});

    exports.entries.push_back(ExportEntry{
        "AppendMenuW", 0,
        [&mgr](Marshaler& m) { mgr.impl_AppendMenuW(m); },
        16});

    exports.entries.push_back(ExportEntry{
        "CallWindowProcW", 0,
        [&mgr](Marshaler& m) { mgr.impl_CallWindowProcW(m); },
        20});

    exports.entries.push_back(ExportEntry{
        "CharNextA", 0,
        [&mgr](Marshaler& m) { mgr.impl_CharNextA(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "CharNextW", 0,
        [&mgr](Marshaler& m) { mgr.impl_CharNextW(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "CharNextExA", 0,
        [&mgr](Marshaler& m) { mgr.impl_CharNextExA(m); },
        12});

    exports.entries.push_back(ExportEntry{
        "CharPrevA", 0,
        [&mgr](Marshaler& m) { mgr.impl_CharPrevA(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "CharPrevW", 0,
        [&mgr](Marshaler& m) { mgr.impl_CharPrevW(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "CheckDlgButton", 0,
        [&mgr](Marshaler& m) { mgr.impl_CheckDlgButton(m); },
        12});

    exports.entries.push_back(ExportEntry{
        "IsDlgButtonChecked", 0,
        [&mgr](Marshaler& m) { mgr.impl_IsDlgButtonChecked(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "SendDlgItemMessageW", 0,
        [&mgr](Marshaler& m) { mgr.impl_SendDlgItemMessageW(m); },
        20});

    exports.entries.push_back(ExportEntry{
        "SendDlgItemMessageA", 0,
        [&mgr](Marshaler& m) { mgr.impl_SendDlgItemMessageA(m); },
        20});

    exports.entries.push_back(ExportEntry{
        "ChangeClipboardChain", 0,
        [&mgr](Marshaler& m) { mgr.impl_ChangeClipboardChain(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "CloseClipboard", 0,
        [&mgr](Marshaler& m) { mgr.impl_CloseClipboard(m); },
        0});

    exports.entries.push_back(ExportEntry{
        "CreatePopupMenu", 0,
        [&mgr](Marshaler& m) { mgr.impl_CreatePopupMenu(m); },
        0});

    exports.entries.push_back(ExportEntry{
        "EmptyClipboard", 0,
        [&mgr](Marshaler& m) { mgr.impl_EmptyClipboard(m); },
        0});

    exports.entries.push_back(ExportEntry{
        "EnableMenuItem", 0,
        [&mgr](Marshaler& m) { mgr.impl_EnableMenuItem(m); },
        12});

    exports.entries.push_back(ExportEntry{
        "FindWindowExA", 0,
        [&mgr](Marshaler& m) { mgr.impl_FindWindowExA(m); },
        16});

    exports.entries.push_back(ExportEntry{
        "FindWindowExW", 0,
        [&mgr](Marshaler& m) { mgr.impl_FindWindowExW(m); },
        16});

    exports.entries.push_back(ExportEntry{
        "EnumWindows", 0,
        [&mgr](Marshaler& m) { mgr.impl_EnumWindows(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "EnumThreadWindows", 0,
        [&mgr](Marshaler& m) { mgr.impl_EnumThreadWindows(m); },
        12});

    exports.entries.push_back(ExportEntry{
        "GetWindowThreadProcessId", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
        mgr.impl_GetWindowThreadProcessId(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "GetDC", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetDC(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "GetDCEx", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetDCEx(m); },
        12});

    exports.entries.push_back(ExportEntry{
        "WindowFromDC", 0,
        [&mgr](Marshaler& m) { mgr.impl_WindowFromDC(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "SetSysColors", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetSysColors(m); },
        12});

    exports.entries.push_back(ExportEntry{
        // DWORD GetMessagePos(void) — no parameters. Do not apply a
        // null-first-argument guard: arg(0) reads RCX, and a zero there is
        // not a missing pointer. The return is the screen point of the last
        // retrieved message, MAKELONG(x, y).
        "GetMessagePos", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetMessagePos(m); },
        0});

    exports.entries.push_back(ExportEntry{
        "GetPropA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetPropA(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "GetPropW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetPropW(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "GetDialogBaseUnits", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetDialogBaseUnits(m); },
        0});

    exports.entries.push_back(ExportEntry{
        "GetSysColor", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetSysColor(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "GetSystemMenu", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetSystemMenu(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "GetSystemMetrics", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetSystemMetrics(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "GetWindowLongW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetWindowLongW(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "GetWindowLongPtrW", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetWindowLongPtrW(m); },
        8});

    exports.entries.push_back(ExportEntry{
        "IsWindow", 0,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { m.set_return(0); return; }
        mgr.impl_IsWindow(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "IsWindowEnabled", 0,
        [&mgr](Marshaler& m) { mgr.impl_IsWindowEnabled(m); },
        4});

    exports.entries.push_back(ExportEntry{
        "LoadBitmapW", 0,
        [&mgr](Marshaler& m) { mgr.impl_LoadBitmapW(m); },
        8});

    // LoadBitmapA — reads ANSI string names (integer IDs still work same as W)
    exports.entries.push_back(ExportEntry{
        "LoadBitmapA", 0,
        [&mgr](Marshaler& m) {
            u64 hInst = m.arg(0);
            GuestAddr name = m.arg_ptr(1);
            u32 handle = 0;
            GuestAddr module_base = static_cast<GuestAddr>(hInst);
            if (module_base == 0) {
                module_base = mgr.rsrc_image_base();
            }

            if (module_base != 0 && name != 0) {
                if (name < 0x10000) {
                    // Integer resource ID — delegate to the W version
                    mgr.impl_LoadBitmapW(m);
                    return;
                }
                // String resource name — read as ANSI
                auto str_res = m.read_ansi_string(name);
                std::string str_name = str_res.is_ok() ? str_res.value() : "";

                if (!str_name.empty()) {
                    std::u16string u16name;
                    for (char c : str_name) {
                        u16name.push_back(static_cast<char16_t>(static_cast<unsigned char>(c)));
                    }
                    if (auto info = try_get_resource_dir_from_module(mgr.memory(), module_base)) {
                        auto res = windowing::find_resource_by_string_name(
                            mgr.memory(), info->image_base, info->rsrc_rva, info->rsrc_size,
                            windowing::RT::BITMAP, u16name.data(), static_cast<u16>(u16name.size()));
                        if (res.data && res.size > 0) {
                            auto bmp_file = dib_to_bmp_file(res.data, res.size);
                            if (!bmp_file.empty()) {
                                handle = mgr.bitmap_host().register_bitmap_bmp(bmp_file);
                            }
                        }
                    }
                }
                VITREO_TRACE("[user32_ext] LoadBitmapA(hInst=0x%llx, name=\"%s\") -> 0x%x\n",
                             static_cast<unsigned long long>(hInst), str_name.c_str(), handle);
            }
            m.set_return(handle);
        },
        8});

    exports.entries.push_back(ExportEntry{
        "LoadImageW", 0,
        [&mgr](Marshaler& m) { mgr.impl_LoadImageW(m); },
        24});

    // LoadImageA — ANSI variant for IMAGE_BITMAP support
    exports.entries.push_back(ExportEntry{
        "LoadImageA", 0,
        [&mgr](Marshaler& m) {
            u64 hInst = m.arg(0);
            GuestAddr name = m.arg_ptr(1);
            u32 type = static_cast<u32>(m.arg(2));
            u32 cx = static_cast<u32>(m.arg(3));
            u32 cy = static_cast<u32>(m.arg(4));
            u32 fuLoad = static_cast<u32>(m.arg(5));

            if (name != 0 && name < 0x10000) {
                // MAKEINTRESOURCE path is encoding-neutral, every type.
                mgr.impl_LoadImageW(m);
                return;
            }
            if (name == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0u);
                return;
            }
            // String names: bitmaps are read here; ANSI icon/cursor names and
            // files are not converted yet and fail loudly.
            if (type != 0) {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!LoadImageA: icon/cursor by ANSI name\n");
                }
                set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
                m.set_return(0u);
                return;
            }

            constexpr u32 LR_LOADFROMFILE = 0x0010;
            auto s = m.read_ansi_string(name);
            std::string win_path = s.is_ok() ? s.value() : "";

            if ((fuLoad & LR_LOADFROMFILE) != 0 && !win_path.empty()) {
                u32 handle = 0;
                auto bytes = read_all_bytes(mgr.vfs(), win_path);
                if (bytes.is_ok() && !bytes.value().empty()) {
                    handle = mgr.bitmap_host().register_bitmap_bmp(bytes.value());
                    if (handle == 0) set_last_error(error_codes::ERROR_INVALID_DATA);
                } else {
                    set_last_error(2u);  // "LoadImageA(missing bmp)=NULL err=2"
                }
                VITREO_TRACE("[user32_ext] LoadImageA(file=\"%s\" type=BITMAP cx=%u cy=%u fu=0x%x) -> 0x%x\n",
                             win_path.c_str(), cx, cy, fuLoad, handle);
                m.set_return(handle);
                return;
            }

            // Named bitmap resource in module
            GuestAddr module_base = static_cast<GuestAddr>(hInst);
            if (module_base == 0) {
                module_base = mgr.rsrc_image_base();
            }

            u32 handle = 0;
            if (!win_path.empty() && module_base != 0) {
                std::u16string u16name;
                u16name.reserve(win_path.size());
                for (char c : win_path) {
                    u16name.push_back(static_cast<char16_t>(static_cast<unsigned char>(c)));
                }

                if (auto info = try_get_resource_dir_from_module(mgr.memory(), module_base)) {
                    auto res = windowing::find_resource_by_string_name(
                        mgr.memory(), info->image_base, info->rsrc_rva, info->rsrc_size,
                        windowing::RT::BITMAP,
                        u16name.data(), static_cast<u16>(u16name.size()));
                    if (res.data && res.size > 0) {
                        auto bmp_file = dib_to_bmp_file(res.data, res.size);
                        if (!bmp_file.empty()) {
                            handle = mgr.bitmap_host().register_bitmap_bmp(bmp_file);
                        }
                    }
                }
            }

            VITREO_TRACE("[user32_ext] LoadImageA(name=\"%s\" hInst=0x%llx type=BITMAP cx=%u cy=%u fu=0x%x) -> 0x%x\n",
                         win_path.c_str(),
                         static_cast<unsigned long long>(hInst),
                         cx, cy, fuLoad, handle);
            m.set_return(handle);
        },
        24});

}

} // namespace vitreo::winapi
