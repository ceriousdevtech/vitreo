// SPDX-License-Identifier: LGPL-2.1-only

/// @file task_dialog.cpp
/// @brief TaskDialog and TaskDialogIndirect (commctrl.h).
///
/// comctl32.lib imports TaskDialogIndirect by ordinal 345, so an application
/// that links it delay-loads comctl32 and resolves that ordinal. With the
/// export absent the delay-load helper failed. Callers answer a failed
/// delay load with an immediate int3 (STATUS_BREAKPOINT) the first time
/// they try to raise a dialog.

#include "winapi/ui/comctl32/comctl32.hpp"
#include "winapi/user32/resources/resources.hpp"
#include "winapi/core/marshaler.hpp"
#include "windowing/model/window_types.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace vitreo::winapi {

namespace {

// TASKDIALOG_COMMON_BUTTON_FLAGS (commctrl.h)
constexpr u32 TDCBF_OK_BUTTON     = 0x0001;
constexpr u32 TDCBF_YES_BUTTON    = 0x0002;
constexpr u32 TDCBF_NO_BUTTON     = 0x0004;
constexpr u32 TDCBF_CANCEL_BUTTON = 0x0008;
constexpr u32 TDCBF_RETRY_BUTTON  = 0x0010;
constexpr u32 TDCBF_CLOSE_BUTTON  = 0x0020;

// Dialog button ids (winuser.h); TaskDialog reports these for common buttons.
constexpr int IDOK = 1, IDCANCEL = 2, IDRETRY = 4, IDYES = 6, IDNO = 7,
              IDCLOSE = 8;

// TASKDIALOG_FLAGS
constexpr u32 TDF_VERIFICATION_FLAG_CHECKED = 0x0100;

// TD_* pseudo-icons are MAKEINTRESOURCE values in the low 16 bits.
constexpr u32 TD_WARNING_ICON = 0xFFFF;         // MAKEINTRESOURCEW(-1)
constexpr u32 TD_ERROR_ICON   = 0xFFFE;         // MAKEINTRESOURCEW(-2)

constexpr u32 S_OK_HR         = 0x00000000;
constexpr u32 E_INVALIDARG_HR = 0x80070057;

/// A guest string that may instead be a MAKEINTRESOURCE ordinal. Task dialog
/// text fields accept either; Vitreo has no message-table lookup, so an
/// ordinal contributes no text rather than being read as a wild pointer.
std::string read_text(Marshaler& m, GuestAddr ptr) {
    if (ptr == 0 || ptr < 0x10000) return {};
    auto text = m.read_wide_string(ptr);
    return text.is_ok() ? text.value() : std::string{};
}

/// Fixed offsets of TASKDIALOGCONFIG, which commctrl.h declares inside
/// pshpack1.h — every field is byte-packed, so nothing is aligned and the
/// layout differs from what a natural-alignment struct would give.
struct ConfigLayout {
    GuestAddr flags, common_buttons, window_title, main_icon, main_instruction,
        content, button_count, buttons, default_button, radio_count,
        radio_buttons, default_radio, verification_text;
};

ConfigLayout layout_for(bool is_64bit) {
    const GuestAddr p = is_64bit ? 8 : 4;   // pointer size
    ConfigLayout l{};
    GuestAddr o = 4;                        // cbSize
    o += p;                                 // hwndParent
    o += p;                                 // hInstance
    l.flags = o;                    o += 4;
    l.common_buttons = o;           o += 4;
    l.window_title = o;             o += p;
    l.main_icon = o;                o += p;
    l.main_instruction = o;         o += p;
    l.content = o;                  o += p;
    l.button_count = o;             o += 4;
    l.buttons = o;                  o += p;
    l.default_button = o;           o += 4;
    l.radio_count = o;              o += 4;
    l.radio_buttons = o;            o += p;
    l.default_radio = o;            o += 4;
    l.verification_text = o;
    return l;
}

u64 read_pointer(Marshaler& m, GuestAddr at) {
    auto value = m.read_ptr(at);
    return value.is_ok() ? value.value() : 0;
}

u32 read_dword(Marshaler& m, GuestAddr at) {
    auto value = m.read_u32(at);
    return value.is_ok() ? value.value() : 0;
}

/// Append the common buttons the caller asked for, in the order commctrl
/// documents them.
void append_common_buttons(u32 common,
                           std::vector<windowing::TaskDialogButton>& out) {
    if ((common & TDCBF_OK_BUTTON) != 0)     out.push_back({IDOK, "OK"});
    if ((common & TDCBF_YES_BUTTON) != 0)    out.push_back({IDYES, "Yes"});
    if ((common & TDCBF_NO_BUTTON) != 0)     out.push_back({IDNO, "No"});
    if ((common & TDCBF_RETRY_BUTTON) != 0)  out.push_back({IDRETRY, "Retry"});
    if ((common & TDCBF_CANCEL_BUTTON) != 0) out.push_back({IDCANCEL, "Cancel"});
    if ((common & TDCBF_CLOSE_BUTTON) != 0)  out.push_back({IDCLOSE, "Close"});
}

} // namespace

u32 ComCtl32Manager::task_dialog_indirect(Marshaler& m, GuestAddr config,
                                          GuestAddr out_button,
                                          GuestAddr out_radio,
                                          GuestAddr out_verification) {
    if (config == 0 || m_resource_mgr == nullptr) {
        return E_INVALIDARG_HR;
    }
    const bool is_64bit = m.cpu_is_64bit();
    const ConfigLayout l = layout_for(is_64bit);

    const u32 flags = read_dword(m, config + l.flags);
    const u32 common = read_dword(m, config + l.common_buttons);
    const std::string title =
        read_text(m, static_cast<GuestAddr>(read_pointer(m, config + l.window_title)));
    const std::string instruction = read_text(
        m, static_cast<GuestAddr>(read_pointer(m, config + l.main_instruction)));
    const std::string content =
        read_text(m, static_cast<GuestAddr>(read_pointer(m, config + l.content)));
    const std::string verification = read_text(
        m, static_cast<GuestAddr>(read_pointer(m, config + l.verification_text)));

    const u32 main_icon =
        static_cast<u32>(read_pointer(m, config + l.main_icon) & 0xFFFFu);
    const bool warning =
        main_icon == TD_WARNING_ICON || main_icon == TD_ERROR_ICON;

    // Custom buttons come first: TASKDIALOG_BUTTON is { int nButtonID; PCWSTR
    // pszButtonText; }, also byte-packed, so the pointer sits at +4 and the
    // stride is 4 + sizeof(pointer).
    std::vector<windowing::TaskDialogButton> buttons;
    const u32 button_count = read_dword(m, config + l.button_count);
    const GuestAddr button_array =
        static_cast<GuestAddr>(read_pointer(m, config + l.buttons));
    const GuestAddr stride = is_64bit ? 12 : 8;
    if (button_array != 0) {
        for (u32 i = 0; i < button_count && i < 64; ++i) {
            const GuestAddr entry = button_array + i * stride;
            const u32 id = read_dword(m, entry);
            const std::string label = read_text(
                m, static_cast<GuestAddr>(read_pointer(m, entry + 4)));
            buttons.push_back({static_cast<int>(id),
                               label.empty() ? std::string{"Button"} : label});
        }
    }
    append_common_buttons(common, buttons);
    if (buttons.empty()) {
        // Learn: with no buttons specified the dialog shows OK.
        buttons.push_back({IDOK, "OK"});
    }

    // nDefaultButton is a button ID, not an index.
    const int default_id = static_cast<int>(read_dword(m, config + l.default_button));
    int default_index = 0;
    for (usize i = 0; i < buttons.size(); ++i) {
        if (buttons[i].id == default_id) {
            default_index = static_cast<int>(i);
            break;
        }
    }

    bool checked = (flags & TDF_VERIFICATION_FLAG_CHECKED) != 0;

    // Always log: a modal dialog with VITREO_TRACE off looks like a hang.
    std::fprintf(stderr,
                 "[taskdialog] [%s] '%s' / '%s' buttons=%zu common=0x%x\n",
                 title.c_str(), instruction.c_str(), content.c_str(),
                 buttons.size(), common);

    const int pressed = m_resource_mgr->task_dialog(
        title, instruction, content, buttons, default_index, verification,
        checked, warning);

    if (out_button != 0) (void)m.write_u32(out_button, static_cast<u32>(pressed));
    if (out_radio != 0) {
        // No radio-button UI yet: report the caller's default, which is what a
        // dialog with the radio group untouched would return.
        (void)m.write_u32(out_radio, read_dword(m, config + l.default_radio));
    }
    if (out_verification != 0) {
        (void)m.write_u32(out_verification, checked ? 1u : 0u);
    }
    return S_OK_HR;
}

u32 ComCtl32Manager::task_dialog(Marshaler& m, GuestAddr title,
                                 GuestAddr instruction, GuestAddr content,
                                 u32 common_buttons, GuestAddr icon,
                                 GuestAddr out_button) {
    if (m_resource_mgr == nullptr) return E_INVALIDARG_HR;

    std::vector<windowing::TaskDialogButton> buttons;
    append_common_buttons(common_buttons, buttons);
    if (buttons.empty()) buttons.push_back({IDOK, "OK"});

    const u32 icon_id = static_cast<u32>(icon) & 0xFFFFu;
    const bool warning = icon_id == TD_WARNING_ICON || icon_id == TD_ERROR_ICON;

    const std::string title_text = read_text(m, title);
    const std::string instruction_text = read_text(m, instruction);
    const std::string content_text = read_text(m, content);
    std::fprintf(stderr, "[taskdialog] TaskDialog [%s] '%s' / '%s'\n",
                 title_text.c_str(), instruction_text.c_str(),
                 content_text.c_str());

    bool checked = false;
    const int pressed = m_resource_mgr->task_dialog(
        title_text, instruction_text, content_text, buttons, 0, {}, checked,
        warning);
    if (out_button != 0) (void)m.write_u32(out_button, static_cast<u32>(pressed));
    return S_OK_HR;
}

} // namespace vitreo::winapi
