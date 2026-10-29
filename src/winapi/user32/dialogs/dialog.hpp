// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file dialog.hpp
/// @brief Dialog box management: CreateDialogParamW, EndDialog, GetDlgItem
///
/// Implements the Win32 dialog box APIs. Dialogs use the built-in "#32770"
/// window class and are backed by the existing HWND and host-control
/// infrastructure.

#include <vitreo/common.hpp>
#include <unordered_map>
#include <utility>
#include "winapi/core/thunk_registry.hpp"
#include "windowing/contracts/control_host.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "windowing/messages/message_queue.hpp"
#include "windowing/messages/message_types.hpp"
#include "windowing/resources/dialog_template.hpp"
#include "winapi/user32/classes/window_class.hpp"
#include <functional>

namespace vitreo { class GuestMemory; }
namespace vitreo::winapi {

/// Set by the ANSI entry points (DialogBoxParamA / CreateDialogParamA) while
/// they run: a template NAME (not MAKEINTRESOURCE) is then an ANSI string.
inline thread_local bool t_template_name_ansi = false;

class Marshaler;
class User32ExtManager;

/// Callback for dispatching messages to a DLGPROC via the interpreter.
/// Args: hwnd, message, wParam, lParam, dlgproc_addr
/// Returns: INT_PTR (BOOL) from the DLGPROC
using DlgProcCallback = std::function<u64(windowing::HWND, u32, u64, u64, u64)>;
using DialogCreationHook =
    std::function<bool(windowing::HWND, u32)>;

/// Manages dialog creation, child control tracking, and dialog lifecycle.
///
/// Each dialog created via CreateDialogParamW receives a real HWND from the
    /// HwndManager and platform-neutral host-control services.
/// Template children are tracked so GetDlgItem returns real handles, or NULL
/// when the control id is absent (MS Learn contract).
class DialogManager {
public:
    [[nodiscard]] windowing::HwndManager& hwnd_mgr() noexcept { return m_hwnd_mgr; }
    /// WM_INITDIALOG message constant
    static constexpr u32 WM_INITDIALOG = 0x0110;

    /// Default dialog dimensions (matches typical installer)
    static constexpr i32 DEFAULT_DLG_WIDTH  = 500;
    static constexpr i32 DEFAULT_DLG_HEIGHT = 380;
    static constexpr i32 DEFAULT_DLG_X      = 200;
    static constexpr i32 DEFAULT_DLG_Y      = 150;

    DialogManager(windowing::HwndManager& hwnd_mgr,
                  WindowClassManager& class_mgr,
                  windowing::ControlHost& control_host,
                  windowing::MessageQueue& queue,
                  GuestMemory& memory);

    /// Set PE resource directory info (called after PE load)
    void set_resource_info(GuestAddr image_base, u32 rsrc_rva, u32 rsrc_size);

    /// Set the DLGPROC callback (for interpreter integration).
    /// This is called to dispatch messages to the guest dialog procedure.
    void set_dlgproc_callback(DlgProcCallback callback);

    /// Set the USER32 creation-hook dispatcher. It is invoked after the HWND
    /// exists but before child creation and WM_INITDIALOG, matching the
    /// HCBT_CREATEWND ordering used by frameworks such as MFC.
    void set_creation_hook(DialogCreationHook callback) {
        m_creation_hook = std::move(callback);
    }

    /// Set a predicate that the modal pump consults each iteration. If the
    /// predicate returns true, the modal dialog is ended with success. This
    /// is used to terminate modal loops when the guest has queued a chain-
    /// load via ShellExecute/CreateProcess: in our serialized chain-load
    /// model the current guest cannot continue running concurrently with
    /// the chained executable, so any active modal pump must release.
    void set_modal_exit_predicate(std::function<bool()> predicate);

    /// Let the modal DialogBox pump synthesize WM_TIMER the same way
    /// GetMessage does. Without this, SetTimer from WM_INITDIALOG never
    /// fires while the modal loop is running.
    void set_user32_ext_manager(User32ExtManager* mgr) { m_user32_ext_mgr = mgr; }

    /// No copy / move (contains references)
    DialogManager(const DialogManager&) = delete;
    DialogManager& operator=(const DialogManager&) = delete;

    /// Create a modeless dialog box.
    /// @param hinstance    Module handle (HMODULE/HINSTANCE) used for resource lookup
    /// @param template_id  Resource ID or name (dialog template — skipped)
    /// @param parent       Parent window HWND
    /// @param dlgproc      Guest address of dialog procedure
    /// @param init_param   LPARAM passed to WM_INITDIALOG
    /// @param post_init    Unused; WM_INITDIALOG is always sent synchronously
    ///                     before the first display (Learn CreateDialogParamW).
    /// @param modal        DialogBoxParamW: show after WM_INITDIALOG even
    ///                     when the template omits WS_VISIBLE.
    /// @return HWND of the created dialog, or NULL_HWND on failure
    windowing::HWND create_dialog(u64 hinstance, u64 template_id,
                                  windowing::HWND parent,
                                  u64 dlgproc, u64 init_param,
                                  bool post_init = false,
                                  bool require_template = false,
                                  bool modal = false);

    /// Run a modal dialog box (DialogBoxParamW).
    /// Creates the dialog, runs a modal message loop dispatching to dlgproc,
    /// and returns when EndDialog is called.
    /// @return The result value passed to EndDialog, or -1 on failure
    i32 dialog_box(u64 hinstance, u64 template_id,
                   windowing::HWND parent,
                   u64 dlgproc, u64 init_param,
                   bool require_template = false);

    /// End a modal dialog.
    /// @param dlg     Dialog HWND
    /// @param result  Value to return from DialogBox
    /// @return TRUE on success
    bool end_dialog(windowing::HWND dlg, i32 result);

    /// Get a child control by ID (MS Learn: NULL if the control does not exist).
    /// @param dlg        Dialog HWND
    /// @param control_id Control identifier
    /// @return Child HWND, or NULL_HWND if the dialog/control is invalid
    windowing::HWND get_dlg_item(windowing::HWND dlg, u32 control_id);

    /// Set the text of a dialog control.
    /// @param dlg        Dialog HWND
    /// @param control_id Control identifier
    /// @param text       New text to set
    /// @return true on success
    bool set_dlg_item_text(windowing::HWND dlg, u32 control_id, const std::wstring& text);

    /// Set the text of a window/control.
    /// @param hwnd Window/control handle
    /// @param text New text to set
    /// @return true on success
    bool set_window_text(windowing::HWND hwnd, const std::wstring& text);

    /// Get the text of a dialog control.
    /// @param dlg        Dialog HWND
    /// @param control_id Control identifier
    /// @param max_count  Max characters to retrieve
    /// @return The control text as wstring (empty if not found)
    std::wstring get_dlg_item_text(windowing::HWND dlg, u32 control_id, u32 max_count);

    /// Get the text of a window/control.
    /// @param hwnd Window/control handle
    /// @return The text as wstring (empty if not found)
    std::wstring get_window_text(windowing::HWND hwnd);

    /// True for a window of this process or a published peer window.
    [[nodiscard]] bool is_window(windowing::HWND hwnd) const;

private:
    windowing::HwndManager& m_hwnd_mgr;
    WindowClassManager&      m_class_mgr;
    windowing::ControlHost&  m_control_host;
    windowing::MessageQueue& m_queue;
    GuestMemory&             m_memory;

    /// DLGPROC callback for interpreter integration
    DlgProcCallback m_dlgproc_callback;
    DialogCreationHook m_creation_hook;

    /// Predicate consulted by the modal pump to decide whether an external
    /// signal (e.g. queued chain-load) should terminate the loop.
    std::function<bool()> m_modal_exit_predicate;

    /// Optional: synthesize WM_TIMER when the modal queue is empty.
    User32ExtManager* m_user32_ext_mgr = nullptr;

    /// PE resource directory info
    GuestAddr m_image_base = 0;
    u32       m_rsrc_rva   = 0;
    u32       m_rsrc_size  = 0;

    /// Modal dialog state: EndDialog sets these
    bool m_modal_ended  = false;
    i32  m_modal_result = 0;

    /// Current modal dialog HWND (set by dialog_box, used as fallback for unknown HWNDs)
    windowing::HWND m_current_modal_hwnd = windowing::NULL_HWND;
    /// Dialog base units (MapDialogRect) of each dialog, from its template
    /// font.
    std::unordered_map<windowing::HWND, std::pair<i32, i32>> m_dialog_base_units;

public:
    /// MapDialogRect's base units for @p dlg; false if it is not a dialog.
    [[nodiscard]] bool dialog_base_units(windowing::HWND dlg, i32& base_x, i32& base_y) const {
        auto it = m_dialog_base_units.find(dlg);
        if (it == m_dialog_base_units.end()) return false;
        base_x = it->second.first;
        base_y = it->second.second;
        return true;
    }

private:

    /// Alias mapping: unknown guest HWND → our real HWND
    /// Populated when guest uses an unrecognized HWND that we can map to a known dialog.
    std::unordered_map<windowing::HWND, windowing::HWND> m_hwnd_aliases;

    /// Tracks the current inner page dialog per panel HWND so we can
    /// hide/destroy the previous page when a new one is created.
    std::unordered_map<windowing::HWND, windowing::HWND> m_panel_page;

    /// Ensure the dialog class "#32770" is registered.
    void ensure_dialog_class();
    bool m_dialog_class_registered = false;

    /// Create child control HWNDs from a parsed dialog template.
    /// @p rsrc_* select the module whose RT_BITMAP / RT_GROUP_ICON are used
    /// for SS_BITMAP / SS_ICON title ordinals (dialog hInstance, not always
    /// the process image).
    void create_controls_from_template(
        windowing::HWND dlg_hwnd,
        const windowing::DialogTemplate& tmpl,
        GuestAddr rsrc_image_base,
        u32 rsrc_rva,
        u32 rsrc_size);

    /// Map of (dialog_hwnd, control_id) → child_hwnd for synthetic controls
    struct DlgItemKey {
        windowing::HWND dlg;
        u32 control_id;
        bool operator==(const DlgItemKey& o) const {
            return dlg == o.dlg && control_id == o.control_id;
        }
    };
    struct DlgItemHash {
        usize operator()(const DlgItemKey& k) const {
            return std::hash<u64>{}((static_cast<u64>(k.dlg) << 32) | k.control_id);
        }
    };
    std::unordered_map<DlgItemKey, windowing::HWND, DlgItemHash> m_dlg_items;
};

// =============================================================================
// Export Table Factory
// =============================================================================

/// Build DllExports for dialog APIs (CreateDialogParamW, EndDialog, GetDlgItem).
[[nodiscard]] DllExports create_dialog_thunks(DialogManager& mgr);

} // namespace vitreo::winapi
