// SPDX-License-Identifier: LGPL-2.1-only

/// @file menu.cpp
/// @brief Menus: the Win32 menu model and the calls that build, query and
/// attach it.
///
/// Contract: Microsoft Learn (CreateMenu ... TrackPopupMenu, MENUITEMINFO,
/// MENUINFO, MENUTEMPLATE / MENUEX_TEMPLATE, winuser.h MF_* / MFT_* / MFS_* /
/// MIIM_* / MIM_*). What Learn leaves open is measured on Windows 11 by
/// tools/oracle_probe/s1win_misc_probe.c ("-- menus", .expected):
///  * an invalid HMENU is ERROR_INVALID_MENU_HANDLE (IsMenu, AppendMenu,
///    GetMenuItemCount -> -1, EnableMenuItem -> -1, DestroyMenu, SetMenu,
///    TrackPopupMenu), a missing item ERROR_MENU_ITEM_NOT_FOUND for the
///    MENUITEMINFO calls and no error for CheckMenuItem / EnableMenuItem
///    (-1), DeleteMenu / RemoveMenu / SetMenuDefaultItem (FALSE);
///  * AppendMenu(MF_POPUP) with a handle that is not a menu adds a plain item
///    with that ID, returns TRUE and sets ERROR_INVALID_MENU_HANDLE;
///  * InsertMenu by a command that is not found appends; a position past the
///    end (or -1) appends;
///  * MENUITEMINFO.cbSize must be sizeof(MENUITEMINFO) (a 32-bit caller may
///    also pass the pre-Windows-2000 size without hbmpItem), MENUINFO.cbSize
///    exactly sizeof(MENUINFO), else ERROR_INVALID_PARAMETER;
///  * GetMenuItemInfo(MIIM_STRING) with no buffer reports the length in cch;
///    with one it copies cch-1 characters and sets cch to the count copied;
///  * CheckMenuItem / EnableMenuItem return the previous MF_CHECKED /
///    MF_GRAYED|MF_DISABLED bits and find commands inside submenus;
///    GetMenuState of a popup item is (item count << 8) | MF_POPUP;
///  * SetMenu refuses a menu made by CreatePopupMenu (ERROR_INVALID_PARAMETER);
///  * GetSystemMenu of a window without WS_SYSMENU is NULL; the window menu
///    has Restore, Move, Size, Minimize, Maximize, a separator and Close.
///
/// The menu bar of a window is presented by the host as the macOS menu bar
/// (CocoaBridge::install_menu_bar); every change to a menu attached to a
/// window is re-published there. Popup-menu tracking (TrackPopupMenu) has no
/// host implementation yet and fails loudly.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/user32/user32_errors.hpp"
#include "winapi/user32/ansi_text.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "windowing/resources/resource_walker.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u32 MF_GRAYED = 0x0001;
constexpr u32 MF_DISABLED = 0x0002;
constexpr u32 MF_BITMAP = 0x0004;
constexpr u32 MF_CHECKED = 0x0008;
constexpr u32 MF_POPUP = 0x0010;
constexpr u32 MF_MENUBARBREAK = 0x0020;
constexpr u32 MF_MENUBREAK = 0x0040;
constexpr u32 MF_HILITE = 0x0080;
constexpr u32 MF_OWNERDRAW = 0x0100;
constexpr u32 MF_BYPOSITION = 0x0400;
constexpr u32 MF_SEPARATOR = 0x0800;
constexpr u32 MF_DEFAULT = 0x1000;
constexpr u32 MF_RIGHTJUSTIFY = 0x4000;
constexpr u32 MFT_SEPARATOR = 0x0800;
constexpr u32 MFT_BITMAP = 0x0004;
constexpr u32 MFT_OWNERDRAW = 0x0100;
constexpr u32 MFT_RADIOCHECK = 0x0200;
constexpr u32 MFS_CHECKED = 0x0008;
constexpr u32 MFS_DEFAULT = 0x1000;
constexpr u32 MIIM_STATE = 0x0001;
constexpr u32 MIIM_ID = 0x0002;
constexpr u32 MIIM_SUBMENU = 0x0004;
constexpr u32 MIIM_CHECKMARKS = 0x0008;
constexpr u32 MIIM_TYPE = 0x0010;
constexpr u32 MIIM_DATA = 0x0020;
constexpr u32 MIIM_STRING = 0x0040;
constexpr u32 MIIM_BITMAP = 0x0080;
constexpr u32 MIIM_FTYPE = 0x0100;
constexpr u32 MIM_MAXHEIGHT = 0x0001;
constexpr u32 MIM_BACKGROUND = 0x0002;
constexpr u32 MIM_HELPID = 0x0004;
constexpr u32 MIM_MENUDATA = 0x0008;
constexpr u32 MIM_STYLE = 0x0010;
constexpr u32 MIM_APPLYTOSUBMENUS = 0x80000000u;
constexpr u32 GMDI_USEDISABLED = 0x0001;
constexpr u32 GMDI_GOINTOPOPUPS = 0x0002;
constexpr u32 ERROR_MENU_ITEM_NOT_FOUND = 1456;
constexpr u32 ERROR_RESOURCE_DATA_NOT_FOUND = 1812;
constexpr u32 ERROR_RESOURCE_NAME_NOT_FOUND = 1814;

/// MENUITEMINFO field offsets (x64 / x86).
struct MiiLayout {
    u32 size, sub, checked, unchecked, data, type_data, cch, bmp_item, ptr;
};
constexpr MiiLayout kMii64{80, 24, 32, 40, 48, 56, 64, 72, 8};
constexpr MiiLayout kMii32{48, 20, 24, 28, 32, 36, 40, 44, 4};

} // namespace

// ---------------------------------------------------------------------------
// Model helpers
// ---------------------------------------------------------------------------

User32ExtManager::MenuRecord* User32ExtManager::menu_record(u32 hmenu) {
    auto it = m_menu_records.find(hmenu);
    return it == m_menu_records.end() ? nullptr : &it->second;
}

bool User32ExtManager::is_menu_handle(u32 hmenu) const {
    std::lock_guard lock(m_menu_mutex);
    return hmenu != 0 && m_menu_records.count(hmenu) != 0;
}

u32 User32ExtManager::new_menu(bool popup) {
    const u32 h = m_next_menu_handle++;
    MenuRecord rec;
    rec.popup = popup;
    m_menu_records[h] = std::move(rec);
    return h;
}

bool User32ExtManager::find_menu_item(u32 hmenu, u32 item, bool by_position, u32& holder,
                                      usize& index, u32 depth) {
    MenuRecord* rec = menu_record(hmenu);
    if (!rec || depth > 32) return false;
    if (by_position) {
        if (item >= rec->items.size()) return false;
        holder = hmenu;
        index = item;
        return true;
    }
    for (usize i = 0; i < rec->items.size(); ++i) {
        const MenuItemRecord& it = rec->items[i];
        if (it.submenu == 0 && it.id == item) {
            holder = hmenu;
            index = i;
            return true;
        }
        if (it.submenu != 0 && find_menu_item(it.submenu, item, false, holder, index, depth + 1)) {
            return true;
        }
    }
    return false;
}

void User32ExtManager::destroy_menu_tree(u32 hmenu, u32 depth) {
    auto it = m_menu_records.find(hmenu);
    if (it == m_menu_records.end() || depth > 32) return;
    std::vector<u32> subs;
    for (const auto& item : it->second.items) {
        if (item.submenu) subs.push_back(item.submenu);
    }
    m_menu_records.erase(it);
    for (u32 sub : subs) destroy_menu_tree(sub, depth + 1);
}

windowing::Win32MenuBar User32ExtManager::menu_to_bar(u32 hmenu, u32 depth) {
    windowing::Win32MenuBar bar;
    MenuRecord* rec = menu_record(hmenu);
    if (!rec || depth > 32) return bar;
    for (const auto& item : rec->items) {
        windowing::Win32MenuItem out;
        out.is_popup = item.submenu != 0;
        out.is_separator = (item.type & MFT_SEPARATOR) != 0;
        out.is_checked = (item.state & MFS_CHECKED) != 0;
        out.is_grayed = (item.state & (MF_GRAYED | MF_DISABLED)) != 0;
        out.id = static_cast<u16>(item.id);
        out.text = item.text;
        if (item.submenu) out.children = menu_to_bar(item.submenu, depth + 1).items;
        bar.items.push_back(std::move(out));
    }
    return bar;
}

bool User32ExtManager::menu_contains(u32 root, u32 hmenu, u32 depth) {
    if (root == hmenu) return true;
    MenuRecord* rec = menu_record(root);
    if (!rec || depth > 32) return false;
    for (const auto& item : rec->items) {
        if (item.submenu && menu_contains(item.submenu, hmenu, depth + 1)) return true;
    }
    return false;
}

void User32ExtManager::menu_changed(u32 hmenu) {
    // Re-publish every menu bar that shows this menu.
    for (const auto& [hwnd, bar] : m_window_menus) {
        if (menu_contains(bar, hmenu, 0)) m_host.install_menu_bar(hwnd, menu_to_bar(bar, 0));
    }
}

u32 User32ExtManager::menu_item_state_flags(const MenuItemRecord& item) const {
    // GetMenuState / CheckMenuItem / EnableMenuItem report MF_* bits.
    u32 flags = item.state & (MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_HILITE | MF_DEFAULT);
    flags |= item.type & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR | MF_MENUBARBREAK |
                          MF_MENUBREAK | MF_RIGHTJUSTIFY);
    // A separator reports itself grayed and disabled (measured,
    // s1win_misc_probe32 getmenustate on a separator: 0x803).
    if (item.type & MFT_SEPARATOR) flags |= MF_GRAYED | MF_DISABLED;
    return flags;
}

// ---------------------------------------------------------------------------
// Creation and destruction
// ---------------------------------------------------------------------------

void User32ExtManager::impl_CreateMenu(Marshaler& m) {
    std::lock_guard lock(m_menu_mutex);
    m.set_return(new_menu(false));
}

void User32ExtManager::impl_CreatePopupMenu(Marshaler& m) {
    std::lock_guard lock(m_menu_mutex);
    m.set_return(new_menu(true));
}

void User32ExtManager::impl_DestroyMenu(Marshaler& m) {
    const u32 hmenu = static_cast<u32>(m.arg(0));
    std::lock_guard lock(m_menu_mutex);
    if (!menu_record(hmenu)) {
        set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
        m.set_return(0u);
        return;
    }
    destroy_menu_tree(hmenu, 0);
    m.set_return(1u);
}

void User32ExtManager::impl_IsMenu(Marshaler& m) {
    const u32 hmenu = static_cast<u32>(m.arg(0));
    if (!is_menu_handle(hmenu)) {
        set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
        m.set_return(0u);
        return;
    }
    m.set_return(1u);
}

// ---------------------------------------------------------------------------
// AppendMenu / InsertMenu / ModifyMenu (legacy MF_* form)
// ---------------------------------------------------------------------------

namespace {
enum class LegacyOp { Append, Insert, Modify };
}

static bool legacy_menu_item(User32ExtManager& mgr, Marshaler& m, LegacyOp op, bool ansi) {
    // AppendMenu(hMenu, uFlags, uIDNewItem, lpNewItem)
    // InsertMenu / ModifyMenu(hMenu, uPosition, uFlags, uIDNewItem, lpNewItem)
    const u32 hmenu = static_cast<u32>(m.arg(0));
    const bool append = op == LegacyOp::Append;
    const u32 position = append ? 0 : static_cast<u32>(m.arg(1));
    const u32 flags = static_cast<u32>(m.arg(append ? 1 : 2));
    const u64 new_id = m.arg(append ? 2 : 3);
    const GuestAddr new_item = m.arg_ptr(append ? 3 : 4);
    return mgr.menu_legacy_edit(m, hmenu, op == LegacyOp::Modify, append, position, flags,
                                new_id, new_item, ansi);
}

bool User32ExtManager::menu_legacy_edit(Marshaler& m, u32 hmenu, bool modify, bool append,
                                        u32 position, u32 flags, u64 new_id,
                                        GuestAddr new_item, bool ansi) {
    std::lock_guard lock(m_menu_mutex);
    MenuRecord* rec = menu_record(hmenu);
    if (!rec) {
        set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
        return false;
    }
    MenuItemRecord item;
    if (flags & MF_SEPARATOR) item.type = MFT_SEPARATOR;
    else if (flags & MF_BITMAP) item.type = MFT_BITMAP;
    else if (flags & MF_OWNERDRAW) item.type = MFT_OWNERDRAW;
    item.type |= flags & (MF_MENUBARBREAK | MF_MENUBREAK | MF_RIGHTJUSTIFY);
    item.state = flags & (MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_HILITE | MF_DEFAULT);
    item.id = static_cast<u32>(new_id);
    if (flags & MF_POPUP) {
        if (menu_record(static_cast<u32>(new_id)) != nullptr) {
            item.submenu = static_cast<u32>(new_id);
        } else {
            // Measured: a plain item with that ID, TRUE, and the error set.
            set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
        }
    }
    if (item.type & MFT_BITMAP) {
        item.bitmap = new_item;
    } else if (item.type & MFT_OWNERDRAW) {
        item.data = new_item;
    } else if (!(item.type & MFT_SEPARATOR) && new_item != 0) {
        auto r = ansi ? m.read_ansi_string(new_item) : m.read_wide_string(new_item);
        if (r.is_ok()) {
            item.text = ansi ? ansi_text::to_wide(r.value()) : utf8_to_wstring(r.value());
        }
    }

    const bool by_position = (flags & MF_BYPOSITION) != 0;
    rec = menu_record(hmenu);
    if (modify) {
        u32 holder = 0;
        usize index = 0;
        if (!find_menu_item(hmenu, position, by_position, holder, index, 0)) {
            return false;
        }
        MenuItemRecord& target = m_menu_records[holder].items[index];
        if (target.submenu && target.submenu != item.submenu) {
            // ModifyMenu replacing a popup item destroys the old submenu
            // (Learn ModifyMenu).
            destroy_menu_tree(target.submenu, 0);
        }
        target = std::move(item);
        menu_changed(holder);
        return true;
    }
    usize at = rec->items.size();
    u32 holder = hmenu;
    if (!append) {
        if (by_position) {
            if (position < rec->items.size()) at = position;
        } else {
            usize index = 0;
            u32 found_holder = 0;
            if (find_menu_item(hmenu, position, false, found_holder, index, 0)) {
                holder = found_holder;
                at = index;
            }
        }
    }
    auto& items = m_menu_records[holder].items;
    items.insert(items.begin() + static_cast<std::ptrdiff_t>(std::min(at, items.size())),
                 std::move(item));
    menu_changed(holder);
    return true;
}

void User32ExtManager::append_menu_impl(Marshaler& m, bool ansi) {
    m.set_return(legacy_menu_item(*this, m, LegacyOp::Append, ansi) ? 1u : 0u);
}
void User32ExtManager::impl_AppendMenuA(Marshaler& m) { append_menu_impl(m, true); }
void User32ExtManager::impl_AppendMenuW(Marshaler& m) { append_menu_impl(m, false); }
void User32ExtManager::impl_InsertMenuW(Marshaler& m) {
    m.set_return(legacy_menu_item(*this, m, LegacyOp::Insert, false) ? 1u : 0u);
}
void User32ExtManager::impl_InsertMenuA(Marshaler& m) {
    m.set_return(legacy_menu_item(*this, m, LegacyOp::Insert, true) ? 1u : 0u);
}
void User32ExtManager::impl_ModifyMenuW(Marshaler& m) {
    m.set_return(legacy_menu_item(*this, m, LegacyOp::Modify, false) ? 1u : 0u);
}
void User32ExtManager::impl_ModifyMenuA(Marshaler& m) {
    m.set_return(legacy_menu_item(*this, m, LegacyOp::Modify, true) ? 1u : 0u);
}

// ---------------------------------------------------------------------------
// DeleteMenu / RemoveMenu
// ---------------------------------------------------------------------------

void User32ExtManager::remove_menu_item(Marshaler& m, bool destroy_submenu) {
    const u32 hmenu = static_cast<u32>(m.arg(0));
    const u32 position = static_cast<u32>(m.arg(1));
    const u32 flags = static_cast<u32>(m.arg(2));
    std::lock_guard lock(m_menu_mutex);
    if (!menu_record(hmenu)) {
        set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
        m.set_return(0u);
        return;
    }
    u32 holder = 0;
    usize index = 0;
    if (!find_menu_item(hmenu, position, (flags & MF_BYPOSITION) != 0, holder, index, 0)) {
        m.set_return(0u);  // measured: FALSE, last error untouched
        return;
    }
    auto& items = m_menu_records[holder].items;
    const u32 sub = items[index].submenu;
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
    if (destroy_submenu && sub) destroy_menu_tree(sub, 0);
    menu_changed(holder);
    m.set_return(1u);
}

void User32ExtManager::impl_DeleteMenu(Marshaler& m) { remove_menu_item(m, true); }
void User32ExtManager::impl_RemoveMenu(Marshaler& m) { remove_menu_item(m, false); }

// ---------------------------------------------------------------------------
// MENUITEMINFO: InsertMenuItem / SetMenuItemInfo / GetMenuItemInfo
// ---------------------------------------------------------------------------

bool User32ExtManager::menu_item_info(Marshaler& m, int op, bool ansi) {
    // op 0 insert, 1 set, 2 get: (hMenu, item, fByPosition, LPMENUITEMINFO)
    const u32 hmenu = static_cast<u32>(m.arg(0));
    const u32 item_arg = static_cast<u32>(m.arg(1));
    const bool by_position = m.arg32(2) != 0;
    const GuestAddr mii = m.arg_ptr(3);
    const MiiLayout& L = m.cpu_is_64bit() ? kMii64 : kMii32;
    auto rd_ptr = [&](u32 off) -> u64 {
        return L.ptr == 8 ? m_memory.read_u64(mii + off).value_or(0)
                          : m_memory.read_u32(mii + off).value_or(0);
    };
    auto wr_ptr = [&](u32 off, u64 v) {
        if (L.ptr == 8) (void)m_memory.write_u64(mii + off, v);
        else (void)m_memory.write_u32(mii + off, static_cast<u32>(v));
    };
    std::lock_guard lock(m_menu_mutex);
    if (!menu_record(hmenu)) {
        set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
        return false;
    }
    const u32 cb = mii ? m_memory.read_u32(mii).value_or(0) : 0;
    const bool legacy32 = !m.cpu_is_64bit() && cb == 44;
    if (mii == 0 || (cb != L.size && !legacy32)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    const u32 mask = m_memory.read_u32(mii + 4).value_or(0);

    if (op == 2) {
        u32 holder = 0;
        usize index = 0;
        if (!find_menu_item(hmenu, item_arg, by_position, holder, index, 0)) {
            set_last_error(ERROR_MENU_ITEM_NOT_FOUND);
            return false;
        }
        const MenuItemRecord& it = m_menu_records[holder].items[index];
        if (mask & (MIIM_FTYPE | MIIM_TYPE)) (void)m_memory.write_u32(mii + 8, it.type);
        if (mask & MIIM_STATE) (void)m_memory.write_u32(mii + 12, it.state);
        if (mask & MIIM_ID) (void)m_memory.write_u32(mii + 16, it.id);
        if (mask & MIIM_SUBMENU) wr_ptr(L.sub, it.submenu);
        if (mask & MIIM_CHECKMARKS) {
            wr_ptr(L.checked, it.checked_bitmap);
            wr_ptr(L.unchecked, it.unchecked_bitmap);
        }
        if (mask & MIIM_DATA) wr_ptr(L.data, it.data);
        if ((mask & MIIM_BITMAP) && !legacy32) wr_ptr(L.bmp_item, it.bitmap_item);
        const bool string_item = !(it.type & (MFT_SEPARATOR | MFT_BITMAP | MFT_OWNERDRAW));
        if ((mask & MIIM_STRING) || ((mask & MIIM_TYPE) && string_item)) {
            const u64 buffer = rd_ptr(L.type_data);
            const u32 cch = m_memory.read_u32(mii + L.cch).value_or(0);
            const std::string ansi_text_value = ansi ? ansi_text::to_ansi(it.text) : std::string{};
            const u32 len = ansi ? static_cast<u32>(ansi_text_value.size())
                                 : static_cast<u32>(it.text.size());
            if (buffer == 0 || cch == 0) {
                (void)m_memory.write_u32(mii + L.cch, len);
            } else {
                const u32 n = std::min(len, cch - 1);
                for (u32 i = 0; i < n; ++i) {
                    if (ansi) (void)m_memory.write_u8(buffer + i, static_cast<u8>(ansi_text_value[i]));
                    else (void)m_memory.write_u16(buffer + i * 2, static_cast<u16>(it.text[i]));
                }
                if (ansi) (void)m_memory.write_u8(buffer + n, 0);
                else (void)m_memory.write_u16(buffer + n * 2, 0);
                (void)m_memory.write_u32(mii + L.cch, n);
            }
        } else if ((mask & MIIM_TYPE) && (it.type & MFT_BITMAP)) {
            wr_ptr(L.type_data, it.bitmap);
        }
        return true;
    }

    MenuItemRecord fresh;
    MenuItemRecord* target = &fresh;
    u32 holder = hmenu;
    usize index = 0;
    if (op == 1) {
        if (!find_menu_item(hmenu, item_arg, by_position, holder, index, 0)) {
            set_last_error(ERROR_MENU_ITEM_NOT_FOUND);
            return false;
        }
        target = &m_menu_records[holder].items[index];
    }
    if (mask & MIIM_SUBMENU) {
        const u32 sub = static_cast<u32>(rd_ptr(L.sub));
        if (sub != 0 && !menu_record(sub)) {
            set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
            return false;
        }
        target->submenu = sub;
    }
    const u32 type = m_memory.read_u32(mii + 8).value_or(0);
    if (mask & MIIM_TYPE) {
        target->type = type;
        const u64 data = rd_ptr(L.type_data);
        if (type & MFT_BITMAP) {
            target->bitmap = data;
        } else if (!(type & (MFT_SEPARATOR | MFT_OWNERDRAW)) && data != 0) {
            auto r = ansi ? m.read_ansi_string(data) : m.read_wide_string(data);
            if (r.is_ok()) target->text = ansi ? ansi_text::to_wide(r.value()) : utf8_to_wstring(r.value());
        }
    }
    if (mask & MIIM_FTYPE) target->type = (target->type & MFT_BITMAP) | (type & ~MFT_BITMAP);
    if (mask & MIIM_STRING) {
        const u64 data = rd_ptr(L.type_data);
        target->text.clear();
        if (data != 0) {
            auto r = ansi ? m.read_ansi_string(data) : m.read_wide_string(data);
            if (r.is_ok()) target->text = ansi ? ansi_text::to_wide(r.value()) : utf8_to_wstring(r.value());
        }
    }
    if ((mask & MIIM_BITMAP) && !legacy32) target->bitmap_item = rd_ptr(L.bmp_item);
    if (mask & MIIM_STATE) target->state = m_memory.read_u32(mii + 12).value_or(0);
    if (mask & MIIM_ID) target->id = m_memory.read_u32(mii + 16).value_or(0);
    if (mask & MIIM_CHECKMARKS) {
        target->checked_bitmap = rd_ptr(L.checked);
        target->unchecked_bitmap = rd_ptr(L.unchecked);
    }
    if (mask & MIIM_DATA) target->data = rd_ptr(L.data);

    if (op == 0) {
        // InsertMenuItem: before the item named by uItem (position or
        // command, the latter searched in submenus), else at the end.
        auto& items = m_menu_records[hmenu].items;
        usize at = items.size();
        holder = hmenu;
        if (by_position) {
            if (item_arg < items.size()) at = item_arg;
        } else {
            u32 found = 0;
            usize idx = 0;
            if (find_menu_item(hmenu, item_arg, false, found, idx, 0)) {
                holder = found;
                at = idx;
            }
        }
        auto& dest = m_menu_records[holder].items;
        dest.insert(dest.begin() + static_cast<std::ptrdiff_t>(std::min(at, dest.size())),
                    std::move(fresh));
    }
    menu_changed(holder);
    return true;
}

void User32ExtManager::impl_InsertMenuItemW(Marshaler& m) { m.set_return(menu_item_info(m, 0, false) ? 1u : 0u); }
void User32ExtManager::impl_InsertMenuItemA(Marshaler& m) { m.set_return(menu_item_info(m, 0, true) ? 1u : 0u); }
void User32ExtManager::impl_SetMenuItemInfoW(Marshaler& m) { m.set_return(menu_item_info(m, 1, false) ? 1u : 0u); }
void User32ExtManager::impl_SetMenuItemInfoA(Marshaler& m) { m.set_return(menu_item_info(m, 1, true) ? 1u : 0u); }
void User32ExtManager::impl_GetMenuItemInfoW(Marshaler& m) { m.set_return(menu_item_info(m, 2, false) ? 1u : 0u); }
void User32ExtManager::impl_GetMenuItemInfoA(Marshaler& m) { m.set_return(menu_item_info(m, 2, true) ? 1u : 0u); }

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

void User32ExtManager::impl_GetMenuItemCount(Marshaler& m) {
    const u32 hmenu = static_cast<u32>(m.arg(0));
    std::lock_guard lock(m_menu_mutex);
    const MenuRecord* rec = menu_record(hmenu);
    if (!rec) {
        set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
        m.set_return(0xFFFFFFFFu);
        return;
    }
    m.set_return(static_cast<u32>(rec->items.size()));
}

void User32ExtManager::impl_GetMenuItemID(Marshaler& m) {
    // UINT GetMenuItemID(HMENU, int nPos): -1 for a popup or no item.
    const u32 hmenu = static_cast<u32>(m.arg(0));
    const i32 pos = static_cast<i32>(m.arg(1));
    std::lock_guard lock(m_menu_mutex);
    const MenuRecord* rec = menu_record(hmenu);
    if (!rec || pos < 0 || static_cast<usize>(pos) >= rec->items.size() ||
        rec->items[static_cast<usize>(pos)].submenu != 0) {
        m.set_return(0xFFFFFFFFu);
        return;
    }
    m.set_return(rec->items[static_cast<usize>(pos)].id);
}

void User32ExtManager::impl_GetSubMenu(Marshaler& m) {
    const u32 hmenu = static_cast<u32>(m.arg(0));
    const i32 pos = static_cast<i32>(m.arg(1));
    std::lock_guard lock(m_menu_mutex);
    const MenuRecord* rec = menu_record(hmenu);
    if (!rec) {
        set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
        m.set_return(0u);
        return;
    }
    if (pos < 0 || static_cast<usize>(pos) >= rec->items.size()) {
        m.set_return(0u);
        return;
    }
    m.set_return(rec->items[static_cast<usize>(pos)].submenu);
}

void User32ExtManager::impl_GetMenuState(Marshaler& m) {
    const u32 hmenu = static_cast<u32>(m.arg(0));
    const u32 item = static_cast<u32>(m.arg(1));
    const u32 flags = static_cast<u32>(m.arg(2));
    std::lock_guard lock(m_menu_mutex);
    if (!menu_record(hmenu)) {
        set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
        m.set_return(0xFFFFFFFFu);
        return;
    }
    u32 holder = 0;
    usize index = 0;
    if (!find_menu_item(hmenu, item, (flags & MF_BYPOSITION) != 0, holder, index, 0)) {
        m.set_return(0xFFFFFFFFu);
        return;
    }
    const MenuItemRecord& it = m_menu_records[holder].items[index];
    u32 state = menu_item_state_flags(it);
    if (it.submenu) {
        const MenuRecord* sub = menu_record(it.submenu);
        const u32 count = sub ? static_cast<u32>(sub->items.size()) : 0;
        state = (count << 8) | MF_POPUP | (state & 0xFF);
    }
    m.set_return(state);
}

u32 User32ExtManager::menu_string(Marshaler& m, bool ansi) {
    // int GetMenuString(HMENU, UINT uIDItem, LPTSTR, int cchMax, UINT flags)
    const u32 hmenu = static_cast<u32>(m.arg(0));
    const u32 item = static_cast<u32>(m.arg(1));
    const GuestAddr buf = m.arg_ptr(2);
    const i32 max = static_cast<i32>(m.arg(3));
    const u32 flags = static_cast<u32>(m.arg(4));
    std::lock_guard lock(m_menu_mutex);
    if (!menu_record(hmenu)) {
        set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
        return 0;
    }
    u32 holder = 0;
    usize index = 0;
    if (!find_menu_item(hmenu, item, (flags & MF_BYPOSITION) != 0, holder, index, 0)) {
        return 0;
    }
    const std::wstring& text = m_menu_records[holder].items[index].text;
    const std::string a = ansi ? ansi_text::to_ansi(text) : std::string{};
    const u32 len = ansi ? static_cast<u32>(a.size()) : static_cast<u32>(text.size());
    if (buf == 0 || max <= 0) return len;
    const u32 n = std::min(len, static_cast<u32>(max) - 1);
    for (u32 i = 0; i < n; ++i) {
        if (ansi) (void)m_memory.write_u8(buf + i, static_cast<u8>(a[i]));
        else (void)m_memory.write_u16(buf + i * 2, static_cast<u16>(text[i]));
    }
    if (ansi) (void)m_memory.write_u8(buf + n, 0);
    else (void)m_memory.write_u16(buf + n * 2, 0);
    return n;
}

void User32ExtManager::impl_GetMenuStringW(Marshaler& m) { m.set_return(menu_string(m, false)); }
void User32ExtManager::impl_GetMenuStringA(Marshaler& m) { m.set_return(menu_string(m, true)); }

// ---------------------------------------------------------------------------
// State: CheckMenuItem / EnableMenuItem / CheckMenuRadioItem / defaults
// ---------------------------------------------------------------------------

void User32ExtManager::impl_CheckMenuItem(Marshaler& m) {
    // DWORD CheckMenuItem(HMENU, UINT uIDCheckItem, UINT uCheck)
    const u32 hmenu = static_cast<u32>(m.arg(0));
    const u32 item = static_cast<u32>(m.arg(1));
    const u32 flags = static_cast<u32>(m.arg(2));
    std::lock_guard lock(m_menu_mutex);
    if (!menu_record(hmenu)) {
        set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
        m.set_return(0xFFFFFFFFu);
        return;
    }
    u32 holder = 0;
    usize index = 0;
    if (!find_menu_item(hmenu, item, (flags & MF_BYPOSITION) != 0, holder, index, 0)) {
        m.set_return(0xFFFFFFFFu);
        return;
    }
    MenuItemRecord& it = m_menu_records[holder].items[index];
    const u32 previous = it.state & MF_CHECKED;
    it.state = (it.state & ~MF_CHECKED) | (flags & MF_CHECKED);
    if (previous != (it.state & MF_CHECKED)) {
        for (const auto& [hwnd, bar] : m_window_menus) {
            if (menu_contains(bar, holder, 0)) {
                m_host.check_menu_item(static_cast<u16>(it.id), (it.state & MF_CHECKED) != 0);
            }
        }
    }
    m.set_return(previous);
}

void User32ExtManager::impl_EnableMenuItem(Marshaler& m) {
    // BOOL EnableMenuItem(HMENU, UINT uIDEnableItem, UINT uEnable): the
    // previous MF_GRAYED | MF_DISABLED bits, or -1.
    const u32 hmenu = static_cast<u32>(m.arg(0));
    const u32 item = static_cast<u32>(m.arg(1));
    const u32 flags = static_cast<u32>(m.arg(2));
    std::lock_guard lock(m_menu_mutex);
    if (!menu_record(hmenu)) {
        set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
        m.set_return(0xFFFFFFFFu);
        return;
    }
    u32 holder = 0;
    usize index = 0;
    if (!find_menu_item(hmenu, item, (flags & MF_BYPOSITION) != 0, holder, index, 0)) {
        m.set_return(0xFFFFFFFFu);
        return;
    }
    MenuItemRecord& it = m_menu_records[holder].items[index];
    const u32 previous = it.state & (MF_GRAYED | MF_DISABLED);
    it.state = (it.state & ~(MF_GRAYED | MF_DISABLED)) | (flags & (MF_GRAYED | MF_DISABLED));
    for (const auto& [hwnd, bar] : m_window_menus) {
        if (menu_contains(bar, holder, 0)) {
            m_host.enable_menu_item(static_cast<u16>(it.id), (it.state & (MF_GRAYED | MF_DISABLED)) == 0);
        }
    }
    m.set_return(previous);
}

void User32ExtManager::impl_CheckMenuRadioItem(Marshaler& m) {
    // BOOL CheckMenuRadioItem(hmenu, first, last, check, flags)
    const u32 hmenu = static_cast<u32>(m.arg(0));
    const u32 first = static_cast<u32>(m.arg(1));
    const u32 last = static_cast<u32>(m.arg(2));
    const u32 check = static_cast<u32>(m.arg(3));
    const u32 flags = static_cast<u32>(m.arg(4));
    std::lock_guard lock(m_menu_mutex);
    MenuRecord* rec = menu_record(hmenu);
    if (!rec) {
        set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
        m.set_return(0u);
        return;
    }
    const bool by_pos = (flags & MF_BYPOSITION) != 0;
    bool found = false;
    for (usize i = 0; i < rec->items.size(); ++i) {
        MenuItemRecord& it = rec->items[i];
        if (it.submenu || (it.type & MFT_SEPARATOR)) continue;
        const u32 key = by_pos ? static_cast<u32>(i) : it.id;
        if (key < first || key > last) continue;
        if (key == check) {
            it.state |= MFS_CHECKED;
            it.type |= MFT_RADIOCHECK;
            found = true;
        } else {
            it.state &= ~MFS_CHECKED;
        }
    }
    if (found) menu_changed(hmenu);
    m.set_return(found ? 1u : 0u);
}

void User32ExtManager::impl_SetMenuDefaultItem(Marshaler& m) {
    // BOOL SetMenuDefaultItem(HMENU, UINT uItem, UINT fByPos)
    const u32 hmenu = static_cast<u32>(m.arg(0));
    const u32 item = static_cast<u32>(m.arg(1));
    const bool by_pos = m.arg32(2) != 0;
    std::lock_guard lock(m_menu_mutex);
    MenuRecord* rec = menu_record(hmenu);
    if (!rec) {
        set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
        m.set_return(0u);
        return;
    }
    if (item == 0xFFFFFFFFu) {
        for (auto& it : rec->items) it.state &= ~MFS_DEFAULT;
        m.set_return(1u);
        return;
    }
    usize target = rec->items.size();
    for (usize i = 0; i < rec->items.size(); ++i) {
        if (by_pos ? i == item : (rec->items[i].submenu == 0 && rec->items[i].id == item)) {
            target = i;
            break;
        }
    }
    if (target == rec->items.size()) {
        m.set_return(0u);  // measured: FALSE, no error, default unchanged
        return;
    }
    for (usize i = 0; i < rec->items.size(); ++i) {
        if (i == target) rec->items[i].state |= MFS_DEFAULT;
        else rec->items[i].state &= ~MFS_DEFAULT;
    }
    m.set_return(1u);
}

void User32ExtManager::impl_GetMenuDefaultItem(Marshaler& m) {
    // UINT GetMenuDefaultItem(HMENU, UINT fByPos, UINT gmdiFlags)
    const u32 hmenu = static_cast<u32>(m.arg(0));
    const bool by_pos = m.arg32(1) != 0;
    const u32 flags = static_cast<u32>(m.arg(2));
    std::lock_guard lock(m_menu_mutex);
    u32 current = hmenu;
    for (u32 depth = 0; depth < 32; ++depth) {
        MenuRecord* rec = menu_record(current);
        if (!rec) {
            if (depth == 0) set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        usize i = 0;
        for (; i < rec->items.size(); ++i) {
            if (rec->items[i].state & MFS_DEFAULT) break;
        }
        if (i == rec->items.size()) {
            m.set_return(0xFFFFFFFFu);
            return;
        }
        const MenuItemRecord& it = rec->items[i];
        if ((it.state & (MF_GRAYED | MF_DISABLED)) && !(flags & GMDI_USEDISABLED)) {
            m.set_return(0xFFFFFFFFu);
            return;
        }
        if (it.submenu && (flags & GMDI_GOINTOPOPUPS)) {
            const MenuRecord* sub = menu_record(it.submenu);
            bool sub_has_default = false;
            if (sub) {
                for (const auto& s : sub->items) sub_has_default = sub_has_default || (s.state & MFS_DEFAULT);
            }
            if (sub_has_default) {
                current = it.submenu;
                continue;
            }
        }
        m.set_return(by_pos ? static_cast<u32>(i) : (it.submenu ? 0xFFFFFFFFu : it.id));
        return;
    }
    m.set_return(0xFFFFFFFFu);
}

void User32ExtManager::impl_SetMenuItemBitmaps(Marshaler& m) {
    // BOOL SetMenuItemBitmaps(HMENU, UINT uPosition, UINT uFlags, HBITMAP, HBITMAP)
    const u32 hmenu = static_cast<u32>(m.arg(0));
    const u32 item = static_cast<u32>(m.arg(1));
    const u32 flags = static_cast<u32>(m.arg(2));
    std::lock_guard lock(m_menu_mutex);
    if (!menu_record(hmenu)) {
        set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
        m.set_return(0u);
        return;
    }
    u32 holder = 0;
    usize index = 0;
    if (!find_menu_item(hmenu, item, (flags & MF_BYPOSITION) != 0, holder, index, 0)) {
        set_last_error(ERROR_MENU_ITEM_NOT_FOUND);
        m.set_return(0u);
        return;
    }
    MenuItemRecord& it = m_menu_records[holder].items[index];
    it.unchecked_bitmap = m.arg(3);
    it.checked_bitmap = m.arg(4);
    m.set_return(1u);
}

void User32ExtManager::impl_GetMenuCheckMarkDimensions(Marshaler& m) {
    // MAKELONG(SM_CXMENUCHECK, SM_CYMENUCHECK) (measured 15 x 15).
    m.set_return(15u | (15u << 16));
}

// ---------------------------------------------------------------------------
// MENUINFO
// ---------------------------------------------------------------------------

bool User32ExtManager::menu_info(Marshaler& m, bool set) {
    const u32 hmenu = static_cast<u32>(m.arg(0));
    const GuestAddr info = m.arg_ptr(1);
    const bool x64 = m.cpu_is_64bit();
    const u32 size = x64 ? 40u : 28u;
    const u32 back_off = 16, help_off = x64 ? 24u : 20u, data_off = x64 ? 32u : 24u;
    std::lock_guard lock(m_menu_mutex);
    MenuRecord* rec = menu_record(hmenu);
    if (!rec) {
        set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
        return false;
    }
    if (info == 0 || m_memory.read_u32(info).value_or(0) != size) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    const u32 mask = m_memory.read_u32(info + 4).value_or(0);
    auto rd_ptr = [&](u32 off) -> u64 {
        return x64 ? m_memory.read_u64(info + off).value_or(0) : m_memory.read_u32(info + off).value_or(0);
    };
    auto wr_ptr = [&](u32 off, u64 v) {
        if (x64) (void)m_memory.write_u64(info + off, v);
        else (void)m_memory.write_u32(info + off, static_cast<u32>(v));
    };
    if (!set) {
        if (mask & MIM_STYLE) (void)m_memory.write_u32(info + 8, rec->style);
        if (mask & MIM_MAXHEIGHT) (void)m_memory.write_u32(info + 12, rec->max_height);
        if (mask & MIM_BACKGROUND) wr_ptr(back_off, rec->background);
        if (mask & MIM_HELPID) (void)m_memory.write_u32(info + help_off, rec->help_id);
        if (mask & MIM_MENUDATA) wr_ptr(data_off, rec->menu_data);
        return true;
    }
    std::function<void(u32, u32)> apply = [&](u32 h, u32 depth) {
        MenuRecord* r = menu_record(h);
        if (!r || depth > 32) return;
        if (mask & MIM_STYLE) r->style = m_memory.read_u32(info + 8).value_or(0);
        if (mask & MIM_MAXHEIGHT) r->max_height = m_memory.read_u32(info + 12).value_or(0);
        if (mask & MIM_BACKGROUND) r->background = rd_ptr(back_off);
        if (mask & MIM_HELPID) r->help_id = m_memory.read_u32(info + help_off).value_or(0);
        if ((mask & MIM_MENUDATA) && depth == 0) r->menu_data = rd_ptr(data_off);
        if (mask & MIM_APPLYTOSUBMENUS) {
            std::vector<u32> subs;
            for (const auto& it : r->items) if (it.submenu) subs.push_back(it.submenu);
            for (u32 s : subs) apply(s, depth + 1);
        }
    };
    apply(hmenu, 0);
    return true;
}

void User32ExtManager::impl_GetMenuInfo(Marshaler& m) { m.set_return(menu_info(m, false) ? 1u : 0u); }
void User32ExtManager::impl_SetMenuInfo(Marshaler& m) { m.set_return(menu_info(m, true) ? 1u : 0u); }

// ---------------------------------------------------------------------------
// Windows: SetMenu / GetMenu / DrawMenuBar / GetSystemMenu
// ---------------------------------------------------------------------------

bool User32ExtManager::set_window_menu(windowing::HWND hwnd, u32 hmenu) {
    if (hmenu != 0) {
        m_window_menus[hwnd] = hmenu;
        std::lock_guard lock(m_menu_mutex);
        m_host.install_menu_bar(hwnd, menu_to_bar(hmenu, 0));
    } else {
        m_window_menus.erase(hwnd);
        m_host.install_menu_bar(hwnd, windowing::Win32MenuBar{});
    }
    // The menu bar is part of the non-client area (SM_CYMENU). If the window
    // had none, the client shrinks by it; the macOS menu bar shows the menu.
    windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (win && !win->has_menu && hmenu != 0) {
        win->has_menu = true;
        constexpr i32 SM_CYMENU = 20;
        if (m_host.has_native_window(hwnd)) {
            const i32 cw = win->client_rect.width();
            i32 ch = win->client_rect.height() - SM_CYMENU;
            if (ch < 1) ch = 1;
            win->client_rect = {0, 0, cw, ch};
            m_host.set_frame(hwnd, windowing::Rect{win->rect.left, win->rect.top,
                                                   win->rect.left + cw, win->rect.top + ch});
        }
    }
    return true;
}

void User32ExtManager::impl_SetMenu(Marshaler& m) {
    // BOOL SetMenu(HWND, HMENU)
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const u32 hmenu = static_cast<u32>(m.arg(1));
    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    if (win->style & windowing::WS::CHILD) {
        // Learn: a child window cannot have a menu.
        set_last_error(user32_errors::ERROR_CHILD_WINDOW_MENU);
        m.set_return(0u);
        return;
    }
    if (hmenu != 0) {
        std::lock_guard lock(m_menu_mutex);
        const MenuRecord* rec = menu_record(hmenu);
        if (!rec) {
            set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
            m.set_return(0u);
            return;
        }
        if (rec->popup) {
            // Measured (setmenu_popup_window): a CreatePopupMenu menu is not
            // a menu bar.
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0u);
            return;
        }
    }
    (void)set_window_menu(hwnd, hmenu);
    m.set_return(1u);
}

void User32ExtManager::impl_GetMenu(Marshaler& m) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    if (!m_hwnd_mgr.is_valid(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    m.set_return(window_menu(hwnd));
}

void User32ExtManager::impl_DrawMenuBar(Marshaler& m) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    if (!m_hwnd_mgr.is_valid(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    if (const u64 bar = window_menu(hwnd)) {
        std::lock_guard lock(m_menu_mutex);
        m_host.install_menu_bar(hwnd, menu_to_bar(static_cast<u32>(bar), 0));
    }
    m.set_return(1u);
}

void User32ExtManager::impl_GetSystemMenu(Marshaler& m) {
    // HMENU GetSystemMenu(HWND, BOOL bRevert)
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const bool revert = m.arg32(1) != 0;
    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (!win) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    std::lock_guard lock(m_menu_mutex);
    auto existing = m_system_menus.find(hwnd);
    if (revert) {
        // Back to the default window menu; the copy is destroyed.
        if (existing != m_system_menus.end()) {
            destroy_menu_tree(existing->second, 0);
            m_system_menus.erase(existing);
        }
        m.set_return(0u);
        return;
    }
    if ((win->style & windowing::WS::SYSMENU) == 0) {
        m.set_return(0u);  // measured: NULL, no error
        return;
    }
    if (existing != m_system_menus.end() && menu_record(existing->second)) {
        m.set_return(existing->second);
        return;
    }
    const u32 h = new_menu(true);
    struct Std { u32 id; const wchar_t* text; };
    static constexpr Std kItems[] = {
        {0xF120, L"&Restore"}, {0xF010, L"&Move"}, {0xF000, L"&Size"},
        {0xF020, L"Mi&nimize"}, {0xF030, L"Ma&ximize"}, {0, nullptr},
        {0xF060, L"&Close\tAlt+F4"},
    };
    MenuRecord* rec = menu_record(h);
    for (const auto& s : kItems) {
        MenuItemRecord item;
        if (s.text == nullptr) {
            item.type = MFT_SEPARATOR;
        } else {
            item.id = s.id;
            item.text = s.text;
        }
        if (s.id == 0xF060) item.state |= MFS_DEFAULT;
        rec->items.push_back(std::move(item));
    }
    m_system_menus[hwnd] = h;
    m.set_return(h);
}

void User32ExtManager::window_menus_destroyed(windowing::HWND hwnd) {
    std::lock_guard lock(m_menu_mutex);
    // Learn DestroyWindow: the window's menu (bar) and window menu are
    // destroyed with it.
    auto sys = m_system_menus.find(hwnd);
    if (sys != m_system_menus.end()) {
        destroy_menu_tree(sys->second, 0);
        m_system_menus.erase(sys);
    }
    auto bar = m_window_menus.find(hwnd);
    if (bar != m_window_menus.end()) {
        destroy_menu_tree(bar->second, 0);
        m_window_menus.erase(bar);
    }
}

// ---------------------------------------------------------------------------
// TrackPopupMenu(Ex)
// ---------------------------------------------------------------------------

void User32ExtManager::track_popup_menu(Marshaler& m, bool ex) {
    // TrackPopupMenu(hMenu, uFlags, x, y, nReserved, hWnd, prcRect)
    // TrackPopupMenuEx(hMenu, uFlags, x, y, hWnd, lptpm)
    const u32 hmenu = static_cast<u32>(m.arg(0));
    const auto hwnd = static_cast<windowing::HWND>(m.arg(ex ? 4 : 5));
    if (!is_menu_handle(hmenu)) {
        set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
        m.set_return(0u);
        return;
    }
    if (!m_hwnd_mgr.is_valid(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        std::fprintf(stderr,
                     "[vitreo] UNIMPLEMENTED user32!TrackPopupMenu%s: no host popup-menu "
                     "tracking; the menu is not shown\n",
                     ex ? "Ex" : "");
    }
    set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
    m.set_return(0u);
}

void User32ExtManager::impl_TrackPopupMenu(Marshaler& m) { track_popup_menu(m, false); }

// ---------------------------------------------------------------------------
// LoadMenu: MENUTEMPLATE (version 0) and MENUEX_TEMPLATE (version 1)
// ---------------------------------------------------------------------------

u32 User32ExtManager::load_menu_from_data(const u8* data, u32 size) {
    if (size < 4) return 0;
    const u16 version = static_cast<u16>(data[0] | (data[1] << 8));
    const u16 header = static_cast<u16>(data[2] | (data[3] << 8));
    auto rd16 = [&](u32 at) -> u32 { return at + 2 <= size ? static_cast<u32>(data[at] | (data[at + 1] << 8)) : 0; };
    auto rd32 = [&](u32 at) -> u32 {
        return at + 4 <= size ? static_cast<u32>(data[at] | (data[at + 1] << 8) | (data[at + 2] << 16) |
                                                 (static_cast<u32>(data[at + 3]) << 24))
                              : 0;
    };
    auto rdstr = [&](u32& at) {
        std::wstring s;
        while (at + 2 <= size) {
            const u32 c = rd16(at);
            at += 2;
            if (c == 0) break;
            s.push_back(static_cast<wchar_t>(c));
        }
        return s;
    };
    u32 pos = 0;
    std::function<u32(bool, u32)> parse = [&](bool popup, u32 depth) -> u32 {
        const u32 h = new_menu(popup);
        if (depth > 32) return h;
        while (pos < size) {
            MenuItemRecord item;
            bool last = false;
            bool has_children = false;
            if (version == 1) {
                // MENUEX_TEMPLATE_ITEM: DWORD type, state, id; WORD resInfo;
                // WCHAR text[]; DWORD-aligned; DWORD helpId if popup.
                pos = (pos + 3) & ~3u;
                item.type = rd32(pos);
                item.state = rd32(pos + 4);
                item.id = rd32(pos + 8);
                const u32 res_info = rd16(pos + 12);
                pos += 14;
                item.text = rdstr(pos);
                last = (res_info & 0x80) != 0;
                has_children = (res_info & 0x01) != 0;
                if (has_children) {
                    pos = (pos + 3) & ~3u;
                    pos += 4;  // dwHelpId
                }
            } else {
                const u32 flags = rd16(pos);
                pos += 2;
                last = (flags & 0x80) != 0;
                has_children = (flags & MF_POPUP) != 0;
                if (!has_children) {
                    item.id = rd16(pos);
                    pos += 2;
                }
                item.text = rdstr(pos);
                item.state = flags & (MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_HILITE | MF_DEFAULT);
                item.type = flags & (MF_BITMAP | MF_OWNERDRAW | MF_MENUBARBREAK | MF_MENUBREAK |
                                     MF_RIGHTJUSTIFY);
                if (!has_children && item.id == 0 && item.text.empty()) item.type |= MFT_SEPARATOR;
            }
            if (has_children) item.submenu = parse(true, depth + 1);
            menu_record(h)->items.push_back(std::move(item));
            if (last) break;
        }
        return h;
    };
    pos = version == 1 ? 4 + header : 4;
    return parse(false, 0);
}

void User32ExtManager::load_menu(Marshaler& m, bool ansi) {
    // HMENU LoadMenu(HINSTANCE, LPCTSTR lpMenuName)
    const u64 hinstance = m.arg(0);
    const u64 name = m.arg(1);
    GuestAddr image_base = m_rsrc_image_base;
    u32 rva = m_rsrc_rva, rsize = m_rsrc_size;
    if (hinstance != 0 && static_cast<GuestAddr>(hinstance) != m_rsrc_image_base) {
        if (auto info = try_get_resource_dir_from_module(m_memory, static_cast<GuestAddr>(hinstance))) {
            image_base = info->image_base;
            rva = info->rsrc_rva;
            rsize = info->rsrc_size;
        } else {
            rva = 0;
        }
    }
    if (rva == 0 || rsize == 0) {
        set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND);
        m.set_return(0u);
        return;
    }
    windowing::ResourceEntry entry{};
    if (name <= 0xFFFF) {
        entry = windowing::find_resource(m_memory, image_base, rva, rsize, windowing::RT::MENU,
                                         static_cast<u16>(name));
    } else {
        auto r = ansi ? m.read_ansi_string(static_cast<GuestAddr>(name))
                      : m.read_wide_string(static_cast<GuestAddr>(name));
        if (r.is_ok()) {
            const std::wstring w = ansi ? ansi_text::to_wide(r.value()) : utf8_to_wstring(r.value());
            std::u16string upper;
            for (wchar_t c : w) upper.push_back(static_cast<char16_t>(c >= L'a' && c <= L'z' ? c - 32 : c));
            entry = windowing::find_resource_by_string_name(m_memory, image_base, rva, rsize,
                                                            windowing::RT::MENU, upper.c_str(),
                                                            static_cast<u16>(upper.size()));
        }
    }
    if (!entry.data || entry.size < 4) {
        set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
        m.set_return(0u);
        return;
    }
    std::lock_guard lock(m_menu_mutex);
    m.set_return(load_menu_from_data(entry.data, entry.size));
}

void User32ExtManager::impl_LoadMenuW(Marshaler& m) { load_menu(m, false); }
void User32ExtManager::impl_LoadMenuA(Marshaler& m) { load_menu(m, true); }

void User32ExtManager::load_and_install_class_menu(windowing::HWND hwnd,
                                                   const std::wstring& menu_name) {
    // CreateWindowEx with no hMenu and a class lpszMenuName: the class menu.
    if (menu_name.empty() || m_rsrc_rva == 0) return;
    windowing::ResourceEntry entry{};
    bool numeric = true;
    for (wchar_t c : menu_name) numeric = numeric && c >= L'0' && c <= L'9';
    if (numeric) {
        const unsigned long v = std::wcstoul(menu_name.c_str(), nullptr, 10);
        if (v > 0 && v <= 0xFFFF) {
            entry = windowing::find_resource(m_memory, m_rsrc_image_base, m_rsrc_rva, m_rsrc_size,
                                             windowing::RT::MENU, static_cast<u16>(v));
        }
    } else {
        std::u16string upper;
        for (wchar_t c : menu_name) upper.push_back(static_cast<char16_t>(c >= L'a' && c <= L'z' ? c - 32 : c));
        entry = windowing::find_resource_by_string_name(m_memory, m_rsrc_image_base, m_rsrc_rva,
                                                        m_rsrc_size, windowing::RT::MENU,
                                                        upper.c_str(), static_cast<u16>(upper.size()));
    }
    if (!entry.data || entry.size < 4) return;
    u32 h = 0;
    {
        std::lock_guard lock(m_menu_mutex);
        h = load_menu_from_data(entry.data, entry.size);
    }
    if (h) (void)set_window_menu(hwnd, h);
}

void User32ExtManager::impl_GetMenuItemRect(Marshaler& m) {
    // BOOL GetMenuItemRect(HWND, HMENU, UINT uItem, LPRECT): the host draws
    // menu bars natively, so no item rectangle exists on the guest screen.
    (void)m;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!GetMenuItemRect: menus are native\n");
    }
    set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
    m.set_return(0u);
}

} // namespace vitreo::winapi
