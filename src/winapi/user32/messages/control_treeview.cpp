// SPDX-License-Identifier: LGPL-2.1-only

/// @file control_treeview.cpp
/// @brief SysTreeView32 native-control message handler.

#include "winapi/user32/user32_internal.hpp"
#include "windowing/resources/resource_walker.hpp"

#include "winapi/user32/messages/message_send_internal.hpp"
#include "winapi/user32/messages/ansi_char_message.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "core/memory/guest_memory.hpp"
#include "windowing/model/diag_flags.hpp"
#include "ipc/ipc_gate.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "winapi/user32/messages/cross_process_send.hpp"
#include "ipc/hwnd_registry.hpp"
#include "ipc/process_registry.hpp"
#include <unistd.h>
#include <algorithm>
#include <pthread.h>
#include <cstdlib>
#include <optional>
#include <string>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

// TreeView messages (TVM_FIRST = 0x1100)
constexpr u32 TVM_FIRST                 = 0x1100;
constexpr u32 TVM_INSERTITEMA           = 0x1100;
constexpr u32 TVM_INSERTITEMW           = 0x1132;
constexpr u32 TVM_DELETEITEM            = 0x1101;
constexpr u32 TVM_EXPAND                = 0x1102;
constexpr u32 TVM_GETITEMA              = 0x110C; // TVM_FIRST+12 (0x1104 is TVM_GETITEMRECT!)
constexpr u32 TVM_GETITEMW              = 0x113E; // TVM_FIRST+62
constexpr u32 TVM_GETNEXTITEM           = 0x110A;
constexpr u32 TVM_SELECTITEM            = 0x110B;
constexpr u32 TVM_LAST                  = 0x11FF;
constexpr u32 TVGN_CARET                = 9;
constexpr u32 TVIF_TEXT                 = 0x0001;
constexpr u32 TVIF_PARAM                = 0x0004;
constexpr u32 TVIF_CHILDREN             = 0x0040;
constexpr u32 WM_NOTIFY_MSG             = 0x004E;
constexpr u32 TVN_SELCHANGEDA           = 0xFFFFFE6Fu; // TVN_FIRST(0u-400) - 1
constexpr u32 TVN_SELCHANGEDW           = 0xFFFFFE6Eu; // TVN_FIRST - 2
constexpr u32 TVN_ITEMEXPANDINGA        = 0xFFFFFE6Bu; // TVN_FIRST - 5
constexpr u32 TVN_ITEMEXPANDINGW        = 0xFFFFFE3Au; // TVN_FIRST - 54

// Lazily-mapped guest scratch page used to marshal NMTREEVIEW for synchronous
// WM_NOTIFY(TVN_SELCHANGED) sends. One page is enough — notifications are sent
// and consumed synchronously, one at a time.
static GuestAddr tv_notify_scratch(Marshaler& m) {
    static GuestAddr s = 0;
    if (s == 0) {
        GuestAddr base = m.memory().find_free_gap(static_cast<GuestAddr>(0x3F000000), 4096);
        if (base != 0 &&
            m.memory().map_region(base, 4096, MemoryProtection::ReadWrite).is_ok()) {
            s = base;
        }
    }
    return s;
}

// SysTreeView32 message handler shared by SendMessageA and SendMessageW (PuTTY is
// ANSI and uses SendMessageA, so the handling cannot live only in the W thunk).
// Returns true and sets the return value if the message was a TreeView message
// targeting a native (wndproc==0) treeview control.
bool try_handle_treeview(Marshaler& m, MessageSendManager& msm,
                         windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam) {
    if (msg < TVM_FIRST || msg > TVM_LAST || msm.wndproc_for(hwnd) != 0) return false;
    if (windowing::diag::win_diag()) {
        std::fprintf(stderr, "[tv] msg=0x%x hwnd=0x%x wParam=0x%llx lParam=0x%llx\n",
                     msg, hwnd, (unsigned long long)wParam, (unsigned long long)lParam);
    }
    const bool is64 = m.cpu_is_64bit();
    const u32 PTRSZ      = is64 ? 8u  : 4u;
    const u32 IT_HITEM   = is64 ? 8u  : 4u;   // TVITEM field offsets (bitness-aware)
    const u32 IT_PSZTEXT = is64 ? 24u : 16u;
    const u32 IT_CHILDREN = is64 ? 44u : 32u;
    const u32 IT_LPARAM  = is64 ? 48u : 36u;

    auto read_ptr = [&](GuestAddr a) -> u64 {
        if (is64) { auto r = m.read_u64(a); return r.is_ok() ? r.value() : 0; }
        auto r = m.read_u32(a); return r.is_ok() ? r.value() : 0;
    };
    auto write_ptr = [&](GuestAddr a, u64 v) {
        if (is64) (void)m.write_u64(a, v); else (void)m.write_u32(a, static_cast<u32>(v));
    };

    if ((msg == TVM_INSERTITEMW || msg == TVM_INSERTITEMA) && lParam != 0) {
        // TVINSERTSTRUCT: hParent@0, hInsertAfter@PTRSZ, item@(2*PTRSZ)
        GuestAddr ins  = static_cast<GuestAddr>(lParam);
        u64 h_parent   = read_ptr(ins);
        GuestAddr item = ins + 2u * PTRSZ;
        u64 psz        = read_ptr(item + IT_PSZTEXT);
        u64 it_lparam  = read_ptr(item + IT_LPARAM);
        u32 mask = 0;
        u32 child_count = 0;
        if (auto result = m.read_u32(item); result.is_ok()) mask = result.value();
        if (auto result = m.read_u32(item + IT_CHILDREN); result.is_ok()) {
            child_count = result.value();
        }
        std::wstring text;
        if (psz != 0) {
            if (msg == TVM_INSERTITEMW) {
                auto r = m.read_wide_string(static_cast<GuestAddr>(psz));
                if (r.is_ok()) text = message_send_utf8_to_wstring(r.value());
            } else {
                auto r = m.read_ansi_string(static_cast<GuestAddr>(psz));
                if (r.is_ok()) text = message_send_utf8_to_wstring(r.value());
            }
        }
        u64 h_item = msm.bridge().treeview_insert_item(
            hwnd, h_parent, text, it_lparam,
            (mask & TVIF_CHILDREN) != 0 && child_count != 0);
        m.set_return(h_item);
        return true;
    }

    if ((msg == TVM_GETITEMW || msg == TVM_GETITEMA) && lParam != 0) {
        GuestAddr item = static_cast<GuestAddr>(lParam);
        u64 h_item = read_ptr(item + IT_HITEM);
        std::wstring text; u64 lp = 0;
        if (msm.bridge().treeview_get_item(hwnd, h_item, &text, &lp)) {
            u32 mask = 0; { auto r = m.read_u32(item); if (r.is_ok()) mask = r.value(); }
            if (mask & TVIF_PARAM) write_ptr(item + IT_LPARAM, lp);
            m.set_return(1u);
        } else {
            m.set_return(0u);
        }
        return true;
    }

    if (msg == TVM_DELETEITEM) {
        m.set_return(msm.bridge().treeview_delete_item(hwnd, lParam) ? 1u : 0u);
        return true;
    }

    if (msg == TVM_GETNEXTITEM) {
        m.set_return(wParam == TVGN_CARET
                     ? msm.bridge().treeview_get_selection(hwnd) : 0u);
        return true;
    }

    if (msg == TVM_EXPAND && lParam != 0) {
        const u64 h_item = lParam;
        windowing::HWND parent = 0;
        u32 ctrl_id = 0;
        if (msm.bridge().treeview_notify_target(hwnd, &parent, &ctrl_id) && parent != 0) {
            std::wstring text;
            u64 item_lparam = 0;
            msm.bridge().treeview_get_item(hwnd, h_item, &text, &item_lparam);
            GuestAddr nm = tv_notify_scratch(m);
            if (nm != 0) {
                for (u32 offset = 0; offset < 160; offset += 8) {
                    (void)m.write_u64(nm + offset, 0);
                }
                if (is64) {
                    (void)m.write_u64(nm + 0, static_cast<u64>(hwnd));
                    (void)m.write_u64(nm + 8, static_cast<u64>(ctrl_id));
                    (void)m.write_u32(nm + 16, TVN_ITEMEXPANDINGW);
                    (void)m.write_u32(nm + 24, static_cast<u32>(wParam));
                    (void)m.write_u32(nm + 96, TVIF_PARAM | 0x8u | 0x1u);
                    (void)m.write_u64(nm + 104, h_item);
                    (void)m.write_u64(nm + 144, item_lparam);
                } else {
                    (void)m.write_u32(nm + 0, static_cast<u32>(hwnd));
                    (void)m.write_u32(nm + 4, ctrl_id);
                    (void)m.write_u32(nm + 8, TVN_ITEMEXPANDINGA);
                    (void)m.write_u32(nm + 12, static_cast<u32>(wParam));
                    (void)m.write_u32(nm + 56, TVIF_PARAM | 0x8u | 0x1u);
                    (void)m.write_u32(nm + 60, static_cast<u32>(h_item));
                    (void)m.write_u32(nm + 92, static_cast<u32>(item_lparam));
                }
                (void)msm.send_message(parent, WM_NOTIFY_MSG,
                                       static_cast<u64>(ctrl_id), static_cast<u64>(nm));
            }
        }
        m.set_return(1u);
        return true;
    }

    if (msg == TVM_SELECTITEM) {
        u64 h_item = static_cast<u64>(lParam);
        // Windows fires TVN_SELCHANGED only when the selection actually changes.
        // Firing unconditionally risks unbounded re-entrancy: PuTTY's WM_NOTIFY
        // handler can re-select the same item, which would notify again forever.
        u64 prev = msm.bridge().treeview_get_selection(hwnd);
        msm.bridge().treeview_select_item(hwnd, h_item);
        windowing::HWND parent = 0; u32 ctrl_id = 0;
        if (h_item != prev && h_item != 0 &&
            msm.bridge().treeview_notify_target(hwnd, &parent, &ctrl_id) && parent != 0) {
            std::wstring t; u64 it_lparam = 0;
            msm.bridge().treeview_get_item(hwnd, h_item, &t, &it_lparam);
            GuestAddr nm = tv_notify_scratch(m);
            if (nm != 0) {
                for (u32 i = 0; i < 160; i += 8) (void)m.write_u64(nm + i, 0);
                if (is64) {
                    // NMHDR: hwndFrom@0 idFrom@8 code@16; itemNew@88
                    (void)m.write_u64(nm + 0,   static_cast<u64>(hwnd));
                    (void)m.write_u64(nm + 8,   static_cast<u64>(ctrl_id));
                    (void)m.write_u32(nm + 16,  TVN_SELCHANGEDW);
                    (void)m.write_u32(nm + 88,  TVIF_PARAM | 0x8u | 0x1u); // PARAM|STATE|HANDLE
                    (void)m.write_u64(nm + 96,  h_item);                   // itemNew.hItem
                    (void)m.write_u32(nm + 104, 2u);                       // TVIS_SELECTED
                    (void)m.write_u64(nm + 136, it_lparam);                // itemNew.lParam
                } else {
                    // NMHDR=12; itemNew@56 (itemOld@16, TVITEM x86=40)
                    (void)m.write_u32(nm + 0,  static_cast<u32>(hwnd));
                    (void)m.write_u32(nm + 4,  ctrl_id);
                    (void)m.write_u32(nm + 8,  TVN_SELCHANGEDW);
                    (void)m.write_u32(nm + 56, TVIF_PARAM | 0x8u | 0x1u);
                    (void)m.write_u32(nm + 60, static_cast<u32>(h_item));
                    (void)m.write_u32(nm + 64, 2u);
                    (void)m.write_u32(nm + 92, static_cast<u32>(it_lparam));
                }
                // SEND (synchronous), matching Win32: TreeView_SelectItem fires
                // TVN_SELCHANGED synchronously, so the parent's WM_NOTIFY handler
                // builds the panel as part of the SelectItem call. This is a
                // guest->guest call on the same (main) thread — no Cocoa dispatch,
                // so no deadlock. Posting it async instead caused PuTTY to build
                // the panel twice (once inline, once on the late notify) with
                // desynced control tracking, so the old controls were never torn
                // down on later category switches (panels stacked).
                (void)msm.send_message(parent, WM_NOTIFY_MSG,
                                       static_cast<u64>(ctrl_id), static_cast<u64>(nm));
            }
        }
        m.set_return(1u);
        return true;
    }

    m.set_return(0u); // other TVM_* — acknowledge
    return true;
}

} // namespace vitreo::winapi
