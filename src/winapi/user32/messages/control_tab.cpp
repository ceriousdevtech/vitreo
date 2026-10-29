// SPDX-License-Identifier: LGPL-2.1-only

/// @file control_tab.cpp
/// @brief Tab control (SysTabControl32) message dispatch.

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

// Tab control messages (TCM_FIRST = 0x1300)
constexpr u32 TCM_FIRST                 = 0x1300;
constexpr u32 TCM_GETIMAGELIST          = 0x1302;
constexpr u32 TCM_SETIMAGELIST          = 0x1303;
constexpr u32 TCM_GETITEMCOUNT          = 0x1304;
constexpr u32 TCM_GETITEMA              = 0x1305;
constexpr u32 TCM_SETITEMA              = 0x1306;
constexpr u32 TCM_INSERTITEMA           = 0x1307;
constexpr u32 TCM_DELETEITEM            = 0x1308;
constexpr u32 TCM_DELETEALLITEMS        = 0x1309;
constexpr u32 TCM_GETITEMRECT           = 0x130A;
constexpr u32 TCM_GETCURSEL             = 0x130B;
constexpr u32 TCM_SETCURSEL             = 0x130C;
constexpr u32 TCM_ADJUSTRECT            = 0x1328;
constexpr u32 TCM_SETITEMSIZE           = 0x1329;
constexpr u32 TCM_SETPADDING            = 0x132B;
constexpr u32 TCM_GETROWCOUNT           = 0x132C;
constexpr u32 TCM_GETTOOLTIPS           = 0x132D;
constexpr u32 TCM_SETTOOLTIPS           = 0x132E;
constexpr u32 TCM_GETCURFOCUS           = 0x132F;
constexpr u32 TCM_SETCURFOCUS           = 0x1330;
constexpr u32 TCM_SETMINTABWIDTH        = 0x1331;
constexpr u32 TCM_DESELECTALL           = 0x1332;
constexpr u32 TCM_SETEXTENDEDSTYLE      = 0x1334;
constexpr u32 TCM_GETEXTENDEDSTYLE      = 0x1335;
constexpr u32 TCM_GETITEMW              = 0x133C;
constexpr u32 TCM_SETITEMW              = 0x133D;
constexpr u32 TCM_INSERTITEMW           = 0x133E;
constexpr u32 TCM_LAST                  = 0x13FF;

constexpr u32 TCIF_TEXT                 = 0x0001;
constexpr u32 TCIF_IMAGE                = 0x0002;
constexpr u32 TCIF_PARAM                = 0x0008;
constexpr u32 TCIF_STATE                = 0x0010;

bool MessageSendManager::dispatch_tab_control(
    Marshaler& m, windowing::HWND hwnd, u32 msg,
    u64 wParam, u64 lParam, bool ansi) {
    if (msg < TCM_FIRST || msg > TCM_LAST ||
        m_bridge.tab_get_item_count(hwnd) < 0) {
        return false;
    }

    switch (msg) {
        case TCM_GETIMAGELIST: {
            std::lock_guard lock(m_control_state_mutex);
            auto& extra = m_tab_extra[hwnd];
            m.set_return(extra.imagelist);
            return true;
        }

        case TCM_SETIMAGELIST: {
            std::lock_guard lock(m_control_state_mutex);
            auto& extra = m_tab_extra[hwnd];
            // Learn: TCM_SETIMAGELIST returns the previous HIMAGELIST (or NULL).
            const u64 previous = extra.imagelist;
            extra.imagelist = lParam;
            VITREO_TRACE("[send] TCM_SETIMAGELIST hwnd=0x%x himl=0x%llx "
                         "prev=0x%llx\n",
                         hwnd,
                         static_cast<unsigned long long>(lParam),
                         static_cast<unsigned long long>(previous));
            m.set_return(previous);
            return true;
        }

        case TCM_GETITEMCOUNT:
            m.set_return(static_cast<u64>(
                m_bridge.tab_get_item_count(hwnd)));
            return true;

        case TCM_DELETEITEM:
            m.set_return(m_bridge.tab_delete_item(hwnd, static_cast<i32>(wParam)) ? 1u : 0u);
            return true;

        case TCM_DELETEALLITEMS:
            while (m_bridge.tab_get_item_count(hwnd) > 0) {
                if (!m_bridge.tab_delete_item(hwnd,
                        m_bridge.tab_get_item_count(hwnd) - 1)) {
                    m.set_return(0u);
                    return true;
                }
            }
            m.set_return(1u);
            return true;

        case TCM_GETITEMA:
        case TCM_GETITEMW: {
            // Learn: TCM_GETITEM — TRUE on success; TCIF_TEXT fills pszText.
            if (lParam == 0) {
                m.set_return(0);
                return true;
            }
            std::wstring text;
            if (!m_bridge.tab_get_item_text(
                    hwnd, static_cast<i32>(wParam), text)) {
                m.set_return(0);
                return true;
            }

            const GuestAddr item = static_cast<GuestAddr>(lParam);
            const u32 mask = m.read_u32(item).value_or(0);
            const bool is64 = m.cpu_is_64bit();
            const u32 text_offset = is64 ? 16u : 12u;
            const u32 cch_offset = is64 ? 24u : 16u;
            const u32 image_offset = is64 ? 28u : 20u;
            const u32 lparam_offset = is64 ? 32u : 24u;
            const u32 state_offset = 4u;

            if ((mask & TCIF_TEXT) != 0) {
                GuestAddr text_ptr = 0;
                if (is64) {
                    text_ptr = static_cast<GuestAddr>(
                        m.read_u64(item + text_offset).value_or(0));
                } else {
                    text_ptr = static_cast<GuestAddr>(
                        m.read_u32(item + text_offset).value_or(0));
                }
                const i32 cch_max = static_cast<i32>(
                    m.read_u32(item + cch_offset).value_or(0));
                if (text_ptr != 0 && cch_max > 0) {
                    const usize max_chars = static_cast<usize>(cch_max);
                    const usize copy_chars =
                        std::min(text.size(), max_chars - 1);
                    for (usize i = 0; i < copy_chars; ++i) {
                        if (msg == TCM_GETITEMA || ansi) {
                            const wchar_t ch = text[i];
                            const u8 byte = (ch < 0x80)
                                ? static_cast<u8>(ch)
                                : static_cast<u8>('?');
                            (void)m.write_u8(text_ptr + i, byte);
                        } else {
                            (void)m.write_u16(
                                text_ptr + static_cast<GuestAddr>(i * 2),
                                static_cast<u16>(text[i]));
                        }
                    }
                    if (msg == TCM_GETITEMA || ansi) {
                        (void)m.write_u8(text_ptr + copy_chars, 0);
                    } else {
                        (void)m.write_u16(
                            text_ptr + static_cast<GuestAddr>(copy_chars * 2),
                            0);
                    }
                }
            }
            if ((mask & TCIF_IMAGE) != 0) {
                (void)m.write_u32(item + image_offset, 0xFFFFFFFFu);
            }
            if ((mask & TCIF_PARAM) != 0) {
                u64 item_lparam = 0;
                (void)m_bridge.tab_get_item_lparam(
                    hwnd, static_cast<i32>(wParam), item_lparam);
                if (is64) {
                    (void)m.write_u64(item + lparam_offset, item_lparam);
                } else {
                    (void)m.write_u32(item + lparam_offset,
                                      static_cast<u32>(item_lparam));
                }
            }
            if ((mask & TCIF_STATE) != 0) {
                (void)m.write_u32(item + state_offset, 0);
            }
            VITREO_TRACE(
                "[send] TCM_GETITEM hwnd=0x%x index=%d text='%ls' mask=0x%x "
                "-> TRUE\n",
                hwnd, static_cast<i32>(wParam), text.c_str(), mask);
            m.set_return(1u);
            return true;
        }

        case TCM_INSERTITEMA:
        case TCM_INSERTITEMW: {
            std::wstring text;
            u64 item_lparam = 0;
            if (lParam != 0) {
                const GuestAddr item = static_cast<GuestAddr>(lParam);
                const u32 mask = m.read_u32(item).value_or(0);
                const bool is64 = m.cpu_is_64bit();
                const u32 text_offset = is64 ? 16u : 12u;
                const u32 lparam_offset = is64 ? 32u : 24u;
                GuestAddr text_ptr = 0;
                if (is64) {
                    text_ptr = static_cast<GuestAddr>(
                        m.read_u64(item + text_offset).value_or(0));
                } else {
                    text_ptr = static_cast<GuestAddr>(
                        m.read_u32(item + text_offset).value_or(0));
                }
                if ((mask & TCIF_TEXT) != 0 && text_ptr != 0) {
                    const bool read_ansi =
                        msg == TCM_INSERTITEMA || ansi;
                    auto result = read_ansi
                        ? m.read_ansi_string(text_ptr)
                        : m.read_wide_string(text_ptr);
                    if (result.is_ok()) {
                        text = message_send_utf8_to_wstring(result.value());
                    }
                }
                if ((mask & TCIF_PARAM) != 0) {
                    if (is64) {
                        item_lparam =
                            m.read_u64(item + lparam_offset).value_or(0);
                    } else {
                        item_lparam =
                            m.read_u32(item + lparam_offset).value_or(0);
                    }
                }
            }
            const i32 inserted = m_bridge.tab_insert_item(
                hwnd, static_cast<i32>(wParam), text, item_lparam);
            VITREO_TRACE(
                "[send] TCM_INSERTITEM hwnd=0x%x index=%d text='%ls'\n",
                hwnd, inserted, text.c_str());
            m.set_return(static_cast<u64>(static_cast<i64>(inserted)));
            return true;
        }

        case TCM_SETITEMA:
        case TCM_SETITEMW: {
            // Learn: TCM_SETITEM returns TRUE on success.
            if (lParam == 0) {
                m.set_return(0);
                return true;
            }
            const GuestAddr item = static_cast<GuestAddr>(lParam);
            const u32 mask = m.read_u32(item).value_or(0);
            const bool is64 = m.cpu_is_64bit();
            const u32 text_offset = is64 ? 16u : 12u;
            const u32 lparam_offset = is64 ? 32u : 24u;
            std::optional<std::wstring> text;
            std::optional<u64> item_lparam;
            if ((mask & TCIF_TEXT) != 0) {
                GuestAddr text_ptr = 0;
                if (is64) {
                    text_ptr = static_cast<GuestAddr>(
                        m.read_u64(item + text_offset).value_or(0));
                } else {
                    text_ptr = static_cast<GuestAddr>(
                        m.read_u32(item + text_offset).value_or(0));
                }
                if (text_ptr != 0) {
                    const bool read_ansi = msg == TCM_SETITEMA || ansi;
                    auto result = read_ansi
                        ? m.read_ansi_string(text_ptr)
                        : m.read_wide_string(text_ptr);
                    if (result.is_ok()) {
                        text = message_send_utf8_to_wstring(result.value());
                    }
                }
            }
            if ((mask & TCIF_PARAM) != 0) {
                if (is64) {
                    item_lparam =
                        m.read_u64(item + lparam_offset).value_or(0);
                } else {
                    item_lparam =
                        m.read_u32(item + lparam_offset).value_or(0);
                }
            }
            const bool ok = m_bridge.tab_set_item(
                hwnd, static_cast<i32>(wParam), text, item_lparam);
            VITREO_TRACE(
                "[send] TCM_SETITEM hwnd=0x%x index=%d mask=0x%x -> %s\n",
                hwnd, static_cast<i32>(wParam), mask, ok ? "TRUE" : "FALSE");
            m.set_return(ok ? 1u : 0u);
            return true;
        }

        case TCM_GETITEMRECT: {
            windowing::Rect rect{};
            if (lParam == 0 ||
                !m_bridge.tab_get_item_rect(
                    hwnd, static_cast<i32>(wParam), rect)) {
                m.set_return(0);
                return true;
            }
            const GuestAddr output = static_cast<GuestAddr>(lParam);
            const bool wrote =
                m.write_u32(output + 0, static_cast<u32>(rect.left)).is_ok() &&
                m.write_u32(output + 4, static_cast<u32>(rect.top)).is_ok() &&
                m.write_u32(output + 8, static_cast<u32>(rect.right)).is_ok() &&
                m.write_u32(output + 12, static_cast<u32>(rect.bottom)).is_ok();
            m.set_return(wrote ? 1u : 0u);
            return true;
        }

        case TCM_GETCURSEL:
        case TCM_GETCURFOCUS:
            m.set_return(static_cast<u64>(static_cast<i64>(
                m_bridge.tab_get_cur_sel(hwnd))));
            return true;

        case TCM_SETCURSEL:
            m.set_return(static_cast<u64>(static_cast<i64>(
                m_bridge.tab_set_cur_sel(
                    hwnd, static_cast<i32>(wParam)))));
            return true;

        case TCM_SETCURFOCUS:
            (void)m_bridge.tab_set_cur_sel(
                hwnd, static_cast<i32>(wParam));
            m.set_return(0);
            return true;

        case TCM_ADJUSTRECT: {
            if (lParam == 0) {
                m.set_return(0);
                return true;
            }
            const GuestAddr rect = static_cast<GuestAddr>(lParam);
            const i32 left = static_cast<i32>(
                m.read_u32(rect + 0).value_or(0));
            const i32 top = static_cast<i32>(
                m.read_u32(rect + 4).value_or(0));
            const i32 right = static_cast<i32>(
                m.read_u32(rect + 8).value_or(0));
            const i32 bottom = static_cast<i32>(
                m.read_u32(rect + 12).value_or(0));
            const bool expand = wParam != 0;
            (void)m.write_u32(rect + 0, static_cast<u32>(
                left + (expand ? -2 : 2)));
            (void)m.write_u32(rect + 4, static_cast<u32>(
                top + (expand ? -24 : 24)));
            (void)m.write_u32(rect + 8, static_cast<u32>(
                right + (expand ? 2 : -2)));
            (void)m.write_u32(rect + 12, static_cast<u32>(
                bottom + (expand ? 2 : -2)));
            m.set_return(0);
            return true;
        }

        case TCM_SETITEMSIZE: {
            std::lock_guard lock(m_control_state_mutex);
            auto& extra = m_tab_extra[hwnd];
            // Learn: returns previous size as MAKELONG(cx, cy).
            const u32 previous = extra.item_size;
            extra.item_size = static_cast<u32>(lParam);
            VITREO_TRACE("[send] TCM_SETITEMSIZE hwnd=0x%x size=0x%x\n",
                         hwnd, extra.item_size);
            m.set_return(previous);
            return true;
        }

        case TCM_SETPADDING: {
            std::lock_guard lock(m_control_state_mutex);
            auto& extra = m_tab_extra[hwnd];
            // Learn: no return value; lParam = MAKELPARAM(cx, cy).
            extra.padding = static_cast<u32>(lParam);
            VITREO_TRACE("[send] TCM_SETPADDING hwnd=0x%x pad=0x%x\n",
                         hwnd, extra.padding);
            m.set_return(0);
            return true;
        }

        case TCM_GETROWCOUNT: {
            // Learn: row count; single-line tabs report 1 when any items exist.
            const i32 count = m_bridge.tab_get_item_count(hwnd);
            const u64 rows = count > 0 ? 1u : 0u;
            VITREO_TRACE("[send] TCM_GETROWCOUNT hwnd=0x%x -> %llu\n",
                         hwnd, static_cast<unsigned long long>(rows));
            m.set_return(rows);
            return true;
        }

        case TCM_GETTOOLTIPS: {
            std::lock_guard lock(m_control_state_mutex);
            auto& extra = m_tab_extra[hwnd];
            m.set_return(extra.tooltip_hwnd);
            return true;
        }

        case TCM_SETTOOLTIPS: {
            std::lock_guard lock(m_control_state_mutex);
            auto& extra = m_tab_extra[hwnd];
            // Learn: wParam = tooltip HWND; no meaningful return.
            extra.tooltip_hwnd = wParam;
            VITREO_TRACE("[send] TCM_SETTOOLTIPS hwnd=0x%x tip=0x%llx\n",
                         hwnd, static_cast<unsigned long long>(wParam));
            m.set_return(0);
            return true;
        }

        case TCM_GETEXTENDEDSTYLE: {
            std::lock_guard lock(m_control_state_mutex);
            auto& extra = m_tab_extra[hwnd];
            m.set_return(extra.ex_style);
            return true;
        }

        case TCM_SETEXTENDEDSTYLE: {
            std::lock_guard lock(m_control_state_mutex);
            auto& extra = m_tab_extra[hwnd];
            // Learn: wParam masks which bits of lParam apply; returns previous.
            const u32 previous = extra.ex_style;
            const u32 mask = static_cast<u32>(wParam);
            const u32 styles = static_cast<u32>(lParam);
            if (mask == 0) {
                extra.ex_style = styles;
            } else {
                extra.ex_style = (previous & ~mask) | (styles & mask);
            }
            VITREO_TRACE("[send] TCM_SETEXTENDEDSTYLE hwnd=0x%x mask=0x%x "
                         "styles=0x%x -> prev=0x%x\n",
                         hwnd, mask, styles, previous);
            m.set_return(previous);
            return true;
        }

        case TCM_SETMINTABWIDTH: {
            std::lock_guard lock(m_control_state_mutex);
            auto& extra = m_tab_extra[hwnd];
            const i32 previous = extra.min_tab_width;
            extra.min_tab_width = static_cast<i32>(wParam);
            VITREO_TRACE("[send] TCM_SETMINTABWIDTH hwnd=0x%x -> %d (prev %d)\n",
                         hwnd, extra.min_tab_width, previous);
            m.set_return(static_cast<u64>(static_cast<i64>(previous)));
            return true;
        }

        case TCM_DESELECTALL:
            m.set_return(1);
            return true;

        default:
            VITREO_TRACE(
                "[send] TCM msg=0x%x hwnd=0x%x (unhandled)\n", msg, hwnd);
            m.set_return(0);
            return true;
    }
}

} // namespace vitreo::winapi
