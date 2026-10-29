// SPDX-License-Identifier: LGPL-2.1-only

/// @file dialog.cpp
/// @brief Dialog box management implementation — template-driven

#include "winapi/user32/dialogs/dialog.hpp"
#include "winapi/user32/user32_ext.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "windowing/resources/resource_walker.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/pe/parser/pe_structures.hpp"


#include <cstdio>
#include <cstdlib>
#include "windowing/model/window_types.hpp"

/// @file get_dlg_item.cpp
/// @brief get_dlg_item
///
/// Closed cut from dialog.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// get_dlg_item
// =============================================================================

windowing::HWND DialogManager::get_dlg_item(windowing::HWND dlg,
                                            u32 control_id) {
    // Some installers pass NULL/desktop here while expecting the active
    // top-level dialog to be implied.
    if (dlg == windowing::HWND_DESKTOP &&
        m_current_modal_hwnd != windowing::NULL_HWND &&
        m_hwnd_mgr.is_valid(m_current_modal_hwnd)) {
        VITREO_TRACE("[dialog] GetDlgItem: remap desktop dlg=0x%x -> 0x%x\n",
                     dlg, m_current_modal_hwnd);
        dlg = m_current_modal_hwnd;
    }

    // If dlg is invalid, try alias mapping or fallback to current modal dialog
    if (!m_hwnd_mgr.is_valid(dlg)) {
        auto alias_it = m_hwnd_aliases.find(dlg);
        if (alias_it != m_hwnd_aliases.end()) {
            dlg = alias_it->second;
        } else if (m_current_modal_hwnd != windowing::NULL_HWND
                   && m_hwnd_mgr.is_valid(m_current_modal_hwnd)) {
            // Register this unknown HWND as an alias for the current modal dialog
            VITREO_TRACE( "[dialog] GetDlgItem: mapping unknown dlg=0x%x -> modal=0x%x\n",
                         dlg, m_current_modal_hwnd);
            m_hwnd_aliases[dlg] = m_current_modal_hwnd;
            dlg = m_current_modal_hwnd;
        } else {
            return windowing::NULL_HWND;
        }
    }

    // Check if we already have a cached HWND for this control — but VALIDATE it.
    // Dialogs (e.g. PuTTY's config box) rebuild a panel by destroying its controls
    // and creating new ones that REUSE the same control IDs with fresh HWNDs. A
    // stale cache entry would return the destroyed HWND, so the caller (which does
    // GetDlgItem(id)->DestroyWindow to tear the panel down) destroys a dead window
    // and the live control leaks on screen (panels stack). Drop stale entries and
    // re-resolve against the live child list.
    DlgItemKey key{dlg, control_id};
    auto it = m_dlg_items.find(key);
    if (it != m_dlg_items.end()) {
        const auto* cached = m_hwnd_mgr.find(it->second);
        if (cached && cached->ctrl_id == control_id) {
            VITREO_TRACE( "[dialog] GetDlgItem: dlg=0x%x id=%u -> cached hwnd=0x%x\n",
                         dlg, control_id, it->second);
            return it->second;
        }
        m_dlg_items.erase(it); // stale (control destroyed/recreated) — re-search below
    }

    // Search real children of this dialog for a window whose control ID
    // (stored in ctrl_id field) matches the requested ID.
    auto children = m_hwnd_mgr.enum_children(dlg);
    for (auto ch : children) {
        const auto* wd = m_hwnd_mgr.find(ch);
        if (wd && wd->ctrl_id == control_id) {
            // Cache for future lookups
            m_dlg_items[key] = ch;
            VITREO_TRACE( "[dialog] GetDlgItem: dlg=0x%x id=%u -> real child hwnd=0x%x\n",
                         dlg, control_id, ch);
            return ch;
        }
    }

    // MS Learn GetDlgItem: nonexistent control → NULL (not a synthetic HWND).
    // A scan of control ids must be able to observe a miss.
    VITREO_TRACE("[dialog] GetDlgItem: dlg=0x%x id=%u -> NULL (no such control)\n",
                 dlg, control_id);
    return windowing::NULL_HWND;
}

} // namespace vitreo::winapi
