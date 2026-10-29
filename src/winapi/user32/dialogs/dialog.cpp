// SPDX-License-Identifier: LGPL-2.1-only

/// @file dialog.cpp
/// @brief Dialog box management implementation — template-driven

#include "ipc/hwnd_registry.hpp"
#include "winapi/user32/dialogs/dialog.hpp"
#include "winapi/user32/dialogs/dialog_internal.hpp"
#include "winapi/user32/user32_ext.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/user32/user32_errors.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "windowing/resources/resource_walker.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/pe/parser/pe_structures.hpp"


#include <cstdio>
#include <cstdlib>
#include "windowing/model/window_types.hpp"

// Diagnostic: log every CreateDialogParamW (gated by VITREO_ENABLE_DIAG_LOGS)
vitreo::windowing::HWND log_create_dialog(vitreo::windowing::HWND hwnd, vitreo::u32 template_id, vitreo::windowing::HWND parent, vitreo::u64 dlgproc, vitreo::u32 client_w, vitreo::u32 client_h) {
    if (vitreo::winapi::diag_logs_enabled()) {
        fprintf(stderr, "[dlg-diag] CreateDialogParamW: hwnd=0x%x template=%u parent=0x%x dlgproc=0x%llx client=%ux%u\n", (unsigned)hwnd, template_id, (unsigned)parent, (unsigned long long)dlgproc, client_w, client_h);
    }
    return hwnd;
}



#include <cmath>
#include <cstdlib>
#include <unistd.h>  // usleep
#include <optional>
#include <array>



namespace vitreo::winapi {


// =============================================================================
// ANSI / UTF-8 string helpers (CP_ACP ≈ Windows-1252)
// =============================================================================

/// Windows-1252 / CP_ACP bytes 0x80–0x9F → Unicode (0xA0–0xFF map 1:1).
static constexpr wchar_t kCp1252_80_9F[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

static wchar_t cp1252_byte_to_wchar(unsigned char b) {
    if (b < 0x80) return static_cast<wchar_t>(b);
    if (b <= 0x9F) return kCp1252_80_9F[b - 0x80];
    return static_cast<wchar_t>(b);
}

static unsigned char wchar_to_cp1252_byte(wchar_t wc) {
    auto cp = static_cast<uint32_t>(wc);
    if (cp < 0x80) return static_cast<unsigned char>(cp);
    if (cp >= 0xA0 && cp <= 0xFF) return static_cast<unsigned char>(cp);
    for (int i = 0; i < 32; ++i) {
        if (kCp1252_80_9F[i] == static_cast<wchar_t>(cp)) {
            return static_cast<unsigned char>(0x80 + i);
        }
    }
    return static_cast<unsigned char>('?');
}

/// Decode Win32 ANSI (CP_ACP / CP1252) to wstring. Used by *A dialog APIs.
static std::wstring ansi_to_wstring(const std::string& s) {
    std::wstring result;
    result.reserve(s.size());
    for (unsigned char c : s) {
        result.push_back(cp1252_byte_to_wchar(c));
    }
    return result;
}

/// Encode wstring to Win32 ANSI (CP_ACP / CP1252) for *A output buffers.
static std::string wstring_to_ansi(const std::wstring& ws) {
    std::string result;
    result.reserve(ws.size());
    for (wchar_t wc : ws) {
        result.push_back(static_cast<char>(wchar_to_cp1252_byte(wc)));
    }
    return result;
}

/// Decode UTF-8; invalid sequences fall back to CP1252 (common when ANSI
/// buffers were mistakenly passed through a UTF-8 decoder — e.g. ® = 0xAE).
static std::wstring utf8_to_wstring(const std::string& s) {
    std::wstring result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            result.push_back(static_cast<wchar_t>(c));
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size() &&
                   (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) {
            wchar_t wc = static_cast<wchar_t>(((c & 0x1F) << 6) |
                         (static_cast<unsigned char>(s[i + 1]) & 0x3F));
            result.push_back(wc);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size() &&
                   (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80 &&
                   (static_cast<unsigned char>(s[i + 2]) & 0xC0) == 0x80) {
            wchar_t wc = static_cast<wchar_t>(((c & 0x0F) << 12) |
                         ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                         (static_cast<unsigned char>(s[i + 2]) & 0x3F));
            result.push_back(wc);
            i += 3;
        } else {
            result.push_back(cp1252_byte_to_wchar(c));
            ++i;
        }
    }
    return result;
}

// =============================================================================
// Construction
// =============================================================================

DialogManager::DialogManager(windowing::HwndManager& hwnd_mgr,
                             WindowClassManager& class_mgr,
                             windowing::ControlHost& control_host,
                             windowing::MessageQueue& queue,
                             GuestMemory& memory)
    : m_hwnd_mgr{hwnd_mgr}
    , m_class_mgr{class_mgr}
    , m_control_host{control_host}
    , m_queue{queue}
    , m_memory{memory}
{}

void DialogManager::set_resource_info(GuestAddr image_base, u32 rsrc_rva, u32 rsrc_size) {
    m_image_base = image_base;
    m_rsrc_rva   = rsrc_rva;
    m_rsrc_size  = rsrc_size;
}

void DialogManager::set_dlgproc_callback(DlgProcCallback callback) {
    m_dlgproc_callback = std::move(callback);
}

void DialogManager::set_modal_exit_predicate(std::function<bool()> predicate) {
    m_modal_exit_predicate = std::move(predicate);
}

// =============================================================================
// Dialog Class Registration
// =============================================================================

void DialogManager::ensure_dialog_class() {
    // "#32770" is a system class (Learn: About Window Classes); the class
    // manager registers it for the process with the measured registration
    // (s1win_class_probe sysclass #32770), so there is nothing to add here.
    m_dialog_class_registered = true;
}




// =============================================================================
// end_dialog
// =============================================================================

bool DialogManager::end_dialog(windowing::HWND dlg, i32 result) {
    // Measured (s1win_misc_probe enddialog_*): an invalid window is
    // ERROR_INVALID_WINDOW_HANDLE, a window that is not a dialog
    // ERROR_WINDOW_NOT_DIALOG; a second EndDialog replaces the result.
    const windowing::WindowData* win = m_hwnd_mgr.find(dlg);
    if (!win) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        return false;
    }
    if (!win->is_dialog) {
        set_last_error(1420);  // ERROR_WINDOW_NOT_DIALOG
        return false;
    }
    if (diag_logs_enabled()) {
        std::fprintf(stderr, "[btn-diag] EndDialog(hwnd=0x%x, result=%d)\n", dlg, result);
    }
    VITREO_TRACE( "[dialog] EndDialog(hwnd=0x%x, result=%d)\n",
                 dlg, result);

    // Store the result and flag so the modal loop exits.
    // Don't post WM_CLOSE or WM_QUIT — the modal loop checks m_modal_ended
    // directly. Posting WM_CLOSE/WM_QUIT can corrupt state if the DlgProc
    // receives them before the loop naturally exits.
    m_modal_ended  = true;
    m_modal_result = result;

    return true;
}


// =============================================================================
// set_dlg_item_text
// =============================================================================

bool DialogManager::set_dlg_item_text(windowing::HWND dlg, u32 control_id,
                                       const std::wstring& text) {
    if (dlg == windowing::HWND_DESKTOP &&
        m_current_modal_hwnd != windowing::NULL_HWND &&
        m_hwnd_mgr.is_valid(m_current_modal_hwnd)) {
        dlg = m_current_modal_hwnd;
    }

    // Resolve alias if this HWND is unknown
    if (!m_hwnd_mgr.is_valid(dlg)) {
        auto alias_it = m_hwnd_aliases.find(dlg);
        if (alias_it != m_hwnd_aliases.end()) {
            dlg = alias_it->second;
        } else if (m_current_modal_hwnd != windowing::NULL_HWND
                   && m_hwnd_mgr.is_valid(m_current_modal_hwnd)) {
            m_hwnd_aliases[dlg] = m_current_modal_hwnd;
            dlg = m_current_modal_hwnd;
        }
    }
    windowing::HWND ctrl = get_dlg_item(dlg, control_id);
    if (ctrl == windowing::NULL_HWND) {
        return false;
    }
    return m_control_host.set_control_text(ctrl, text);
}

// =============================================================================
// set_window_text
// =============================================================================

bool DialogManager::set_window_text(windowing::HWND hwnd, const std::wstring& text) {
    // Win32 expects a non-NULL HWND; however some installers effectively use
    // hwnd==0 as a shorthand for the active/modal wizard window.
    if (hwnd == windowing::HWND_DESKTOP) {
        hwnd = windowing::NULL_HWND;
    }

    // Resolve unknown/NULL HWND to an alias or the current modal dialog.
    if (!m_hwnd_mgr.is_valid(hwnd)) {
        auto alias_it = m_hwnd_aliases.find(hwnd);
        if (alias_it != m_hwnd_aliases.end()) {
            hwnd = alias_it->second;
        } else if (m_current_modal_hwnd != windowing::NULL_HWND
                   && m_hwnd_mgr.is_valid(m_current_modal_hwnd)) {
            m_hwnd_aliases[hwnd] = m_current_modal_hwnd;
            hwnd = m_current_modal_hwnd;
        }
    }

    // A top-level window is resolved FIRST, before the control path.
    //
    // set_control_text() also accepts a top-level HWND: it pushes the string
    // to the NSWindow title bar and returns true. That is a display-only sink
    // -- nothing reads back out of it, because get_control_text() looks only
    // at the control map. Trying controls first therefore made SetWindowTextW
    // report success, update the visible title, and leave win->title (what
    // GetWindowTextW actually returns) still holding the creation-time title.
    // Verified against Windows: SetWindowTextW then GetWindowTextW round-trips.
    windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (win) {
        win->title = text;
        // Measured (winevent_sequence_probe): a window's text change raises
        // EVENT_OBJECT_NAMECHANGE.
        if (m_user32_ext_mgr != nullptr) {
            m_user32_ext_mgr->notify_win_event(0x800Cu /* EVENT_OBJECT_NAMECHANGE */, hwnd);
        }
        m_control_host.set_title(hwnd, text);
        // Keep the visible title in step for windows the bridge owns.
        (void)m_control_host.set_control_text(hwnd, text);
        return true;
    }
    if (m_control_host.set_control_text(hwnd, text)) {
        return true;
    }
    return false;
}

// =============================================================================
// get_dlg_item_text
// =============================================================================

std::wstring DialogManager::get_dlg_item_text(windowing::HWND dlg, u32 control_id,
                                               u32 max_count) {
    windowing::HWND ctrl = get_dlg_item(dlg, control_id);
    if (ctrl == windowing::NULL_HWND) {
        return {};
    }
    std::wstring text = m_control_host.get_control_text(ctrl);
    if (max_count > 0 && text.size() > static_cast<std::size_t>(max_count - 1)) {
        text.resize(max_count - 1);
    }
    return text;
}

// =============================================================================
// get_window_text
// =============================================================================

std::wstring DialogManager::get_window_text(windowing::HWND hwnd) {
    // Check child controls first
    std::wstring text = m_control_host.get_control_text(hwnd);
    if (!text.empty()) {
        return text;
    }

    // Check top-level windows
    const auto* win = m_hwnd_mgr.find(hwnd);
    if (win) {
        return win->title;
    }
    return {};
}

// =============================================================================
// Thunk Implementations
// =============================================================================

static void impl_CreateDialogParamW(Marshaler& m, DialogManager& mgr) {
    // HINSTANCE is a module base address, 64-bit on x64. Truncating it to 32
    // bits misses every resource in a module based above 4 GiB, and
    // create_dialog then falls back to the main module's resources — a real
    // dialog built from the wrong template. The modal loop then runs on
    // controls that do not match the caller's dialog proc.
    auto hinstance   = m.arg(0);
    auto template_id = m.arg(1);
    auto parent      = static_cast<windowing::HWND>(m.arg(2));
    auto dlgproc     = m.arg(3);
    auto init_param  = m.arg(4);

    if (template_id == 0) {
        // Real Win returns NULL + ERROR_RESOURCE_TYPE_NOT_FOUND when template is null.
        set_last_error(0x715u);
        m.set_return(0);
        return;
    }

    VITREO_TRACE( "[dialog] CreateDialogParamW: template=%llu parent=0x%x dlgproc=0x%llx\n",
                 static_cast<unsigned long long>(template_id), parent,
                 static_cast<unsigned long long>(dlgproc));

    windowing::HWND hwnd = mgr.create_dialog(hinstance, template_id,
                                              parent, dlgproc, init_param);
    VITREO_TRACE( "[hwnd-trace] CreateDialogParamW -> hwnd=0x%x template=%llu parent=0x%x dlgproc=0x%llx\n",
                 hwnd, static_cast<unsigned long long>(template_id), parent,
                 static_cast<unsigned long long>(dlgproc));
    m.set_return(static_cast<u64>(hwnd));
}

static void impl_DialogBoxParamW(Marshaler& m, DialogManager& mgr) {
    // HINSTANCE is a module base address, 64-bit on x64. Truncating it to 32
    // bits misses every resource in a module based above 4 GiB, and
    // create_dialog then falls back to the main module's resources — a real
    // dialog built from the wrong template. The modal loop then runs on
    // controls that do not match the caller's dialog proc.
    auto hinstance   = m.arg(0);
    auto template_id = m.arg(1);
    auto parent      = static_cast<windowing::HWND>(m.arg(2));
    auto dlgproc     = m.arg(3);
    auto init_param  = m.arg(4);

    VITREO_TRACE( "[hwnd-trace] DialogBoxParamW ENTER template=%llu parent=0x%x dlgproc=0x%llx\n",
                 static_cast<unsigned long long>(template_id), parent,
                 static_cast<unsigned long long>(dlgproc));

    i32 result = mgr.dialog_box(hinstance, template_id,
                                 parent, dlgproc, init_param);

    VITREO_TRACE( "[hwnd-trace] DialogBoxParamW EXIT result=%d\n", result);
    m.set_return(static_cast<u64>(static_cast<u32>(result)));
}

static void impl_EndDialog(Marshaler& m, DialogManager& mgr) {
    auto dlg    = static_cast<windowing::HWND>(m.arg(0));
    auto result = static_cast<i32>(m.arg(1));
    VITREO_TRACE( "[hwnd-trace] EndDialog dlg=0x%x result=%d\n", dlg, result);
    bool ok = mgr.end_dialog(dlg, result);
    m.set_return(ok ? 1u : 0u);
}

static void impl_GetDlgItemInt(Marshaler& m, DialogManager& mgr) {
    // UINT GetDlgItemInt(HWND, int nIDDlgItem, BOOL* lpTranslated, BOOL bSigned)
    // Measured (s1win_misc_probe "-- dialog init"): leading blanks are
    // skipped; a '-' only with bSigned ('+' is never accepted); the digits'
    // value is returned even when text follows them, but *lpTranslated is
    // then FALSE; no digits or an out-of-range value is 0 / FALSE; a
    // missing control is ERROR_CONTROL_ID_NOT_FOUND; success sets the last
    // error to 0.
    const auto dlg = static_cast<windowing::HWND>(m.arg(0));
    const auto control_id = static_cast<u32>(m.arg(1));
    const GuestAddr trans_ptr = m.arg_ptr(2);
    const bool is_signed = m.arg32(3) != 0;
    auto finish = [&](u32 value, bool translated) {
        if (trans_ptr != 0) (void)m.write_u32(static_cast<GuestAddr>(trans_ptr), translated ? 1u : 0u);
        m.set_return(value);
    };
    if (mgr.get_dlg_item(dlg, control_id) == windowing::NULL_HWND) {
        set_last_error(1421);  // ERROR_CONTROL_ID_NOT_FOUND
        finish(0, false);
        return;
    }
    const std::wstring text = mgr.get_dlg_item_text(dlg, control_id, 0);
    usize i = 0;
    while (i < text.size() && (text[i] == L' ' || text[i] == L'\t')) ++i;
    bool negative = false;
    if (is_signed && i < text.size() && text[i] == L'-') {
        negative = true;
        ++i;
    }
    const usize digits_start = i;
    u64 value = 0;
    bool overflow = false;
    while (i < text.size() && text[i] >= L'0' && text[i] <= L'9') {
        value = value * 10 + static_cast<u64>(text[i] - L'0');
        if (value > 0xFFFFFFFFull) overflow = true;
        ++i;
    }
    if (i == digits_start || overflow) {
        finish(0, false);
        return;
    }
    if (is_signed) {
        const u64 limit = negative ? 0x80000000ull : 0x7FFFFFFFull;
        if (value > limit) {
            finish(0, false);
            return;
        }
    }
    const u32 result = negative ? static_cast<u32>(-static_cast<i64>(value)) : static_cast<u32>(value);
    const bool translated = i == text.size();
    if (translated) set_last_error(0);
    finish(result, translated);
}

bool DialogManager::is_window(windowing::HWND hwnd) const {
    if (hwnd == windowing::NULL_HWND) return false;
    return m_hwnd_mgr.find(hwnd) != nullptr || ipc::hwnd_registry_is_window(static_cast<u32>(hwnd));
}

static void impl_GetDlgItem(Marshaler& m, DialogManager& mgr) {
    auto dlg = static_cast<windowing::HWND>(m.arg(0));
    auto id  = static_cast<u32>(m.arg(1));
    windowing::HWND child = mgr.get_dlg_item(dlg, id);
    if (child == windowing::NULL_HWND) {
        // Measured (s1win_api_probe getdlgitem_*): a missing control (a
        // grandchild does not count) is ERROR_CONTROL_ID_NOT_FOUND, an
        // invalid or NULL dialog ERROR_INVALID_WINDOW_HANDLE.
        set_last_error(mgr.hwnd_mgr().is_valid(dlg)
                           ? user32_errors::ERROR_CONTROL_ID_NOT_FOUND
                           : error_codes::ERROR_INVALID_WINDOW_HANDLE);
    }
    m.set_return(static_cast<u64>(child));
}

static void impl_SetDlgItemTextW(Marshaler& m, DialogManager& mgr) {
    auto dlg = static_cast<windowing::HWND>(m.arg(0));
    auto id  = static_cast<u32>(m.arg(1));
    auto text_ptr = m.arg_ptr(2);

    std::wstring text;
    if (text_ptr != 0) {
        auto result = m.read_wide_string(text_ptr);
        if (result.is_ok()) {
            // read_wide_string returns UTF-8
            text = utf8_to_wstring(result.value());
        }
    }

    VITREO_TRACE( "[dialog] SetDlgItemTextW: dlg=0x%x id=%u text_len=%zu\n",
                 dlg, id, text.size());
    bool ok = mgr.set_dlg_item_text(dlg, id, text);
    m.set_return(ok ? 1u : 0u);
}

static void impl_SetWindowTextW(Marshaler& m, DialogManager& mgr) {
    auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    auto text_ptr = m.arg_ptr(1);

    std::wstring text;
    if (text_ptr != 0) {
        auto result = m.read_wide_string(text_ptr);
        if (result.is_ok()) {
            text = utf8_to_wstring(result.value());
        }
    }

    VITREO_TRACE( "[dialog] SetWindowTextW: hwnd=0x%x text_len=%zu\n",
                 hwnd, text.size());
    // A handle that is not a window: TRUE, error untouched
    // (s3gdi_usermisc_probe "SetWindowTextW(bad)=1 err=57005").
    if (!mgr.is_window(hwnd)) {
        m.set_return(1u);
        return;
    }
    bool ok = mgr.set_window_text(hwnd, text);
    m.set_return(ok ? 1u : 0u);
}

static void impl_GetDlgItemTextW(Marshaler& m, DialogManager& mgr) {
    auto dlg     = static_cast<windowing::HWND>(m.arg(0));
    auto id      = static_cast<u32>(m.arg(1));
    auto buf_ptr = m.arg_ptr(2);
    auto cch_max = static_cast<u32>(m.arg(3));

    // No such control: 0 with ERROR_CONTROL_ID_NOT_FOUND and an empty
    // buffer (s3gdi_usermisc_probe "GetDlgItemTextW(missing)=0 err=1421").
    if (mgr.get_dlg_item(dlg, id) == windowing::NULL_HWND) {
        if (buf_ptr != 0 && cch_max > 0) (void)m.write_u16(buf_ptr, 0);
        set_last_error(1421u);  // ERROR_CONTROL_ID_NOT_FOUND
        m.set_return(0u);
        return;
    }
    std::wstring text = mgr.get_dlg_item_text(dlg, id, cch_max);
    // At most cchMax - 1 characters and the NUL ("GetDlgItemTextW(cch3)=2").
    if (cch_max == 0) {
        m.set_return(0u);
        return;
    }
    if (text.size() > static_cast<std::size_t>(cch_max - 1)) text.resize(cch_max - 1);
    std::string utf8 = wstring_to_utf8(text);

    VITREO_TRACE( "[dialog] GetDlgItemTextW: dlg=0x%x id=%u max=%u -> \"%s\" (%zu chars)\n",
                 dlg, id, cch_max, utf8.c_str(), text.size());

    if (buf_ptr != 0) (void)m.write_wide_string(buf_ptr, utf8);

    m.set_return(static_cast<u64>(text.size()));
}

static void impl_GetWindowTextW(Marshaler& m, DialogManager& mgr) {
    auto hwnd    = static_cast<windowing::HWND>(m.arg(0));
    auto buf_ptr = m.arg_ptr(1);
    auto cch_max = static_cast<u32>(m.arg(2));
    // s3gdi_usermisc_probe: a bad handle is 0 with ERROR_INVALID_WINDOW_HANDLE,
    // a zero-length buffer 0 with nothing written.
    if (!mgr.is_window(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    if (cch_max == 0) {
        m.set_return(0u);
        return;
    }

    std::wstring text = mgr.get_window_text(hwnd);
    if (cch_max > 0 && text.size() > static_cast<std::size_t>(cch_max - 1)) {
        text.resize(cch_max - 1);
    }
    std::string utf8 = wstring_to_utf8(text);

    VITREO_TRACE( "[dialog] GetWindowTextW: hwnd=0x%x max=%u -> \"%s\" (%zu chars)\n",
                 hwnd, cch_max, utf8.c_str(), text.size());

    if (buf_ptr != 0 && cch_max > 0) {
        if (!text.empty()) {
            (void)m.write_wide_string(buf_ptr, utf8);
        } else {
            (void)m.write_wide_string(buf_ptr, "");
        }
    }

    m.set_return(static_cast<u64>(text.size()));
}

static void impl_InternalGetWindowText(Marshaler& m, DialogManager& mgr) {
    // InternalGetWindowText retrieves USER's stored window text directly and
    // always writes UTF-16; unlike GetWindowText, it does not send WM_GETTEXT.
    // Vitreo's window/control model is the owner of that stored text, so this
    // deliberately shares the direct model read used by GetWindowTextW.
    auto hwnd    = static_cast<windowing::HWND>(m.arg(0));
    auto buf_ptr = m.arg_ptr(1);
    auto cch_max = static_cast<u32>(m.arg(2));

    std::wstring text = mgr.get_window_text(hwnd);
    if (cch_max > 0 && text.size() > static_cast<std::size_t>(cch_max - 1)) {
        text.resize(cch_max - 1);
    }
    if (buf_ptr != 0 && cch_max > 0) {
        (void)m.write_wide_string(buf_ptr, wstring_to_utf8(text));
    }
    m.set_return(static_cast<u64>(text.size()));
}

// =============================================================================
// ANSI (A-suffix) Wrappers
// =============================================================================

static void impl_CreateDialogParamA(Marshaler& m, DialogManager& mgr) {
    // As W; a template name is an ANSI string.
    t_template_name_ansi = true;
    impl_CreateDialogParamW(m, mgr);
    t_template_name_ansi = false;
}

static void impl_DialogBoxParamA(Marshaler& m, DialogManager& mgr) {
    t_template_name_ansi = true;
    impl_DialogBoxParamW(m, mgr);
    t_template_name_ansi = false;
}

static void impl_SetDlgItemTextA(Marshaler& m, DialogManager& mgr) {
    auto dlg = static_cast<windowing::HWND>(m.arg(0));
    auto id  = static_cast<u32>(m.arg(1));
    auto text_ptr = m.arg_ptr(2);

    std::wstring text;
    if (text_ptr != 0) {
        auto result = m.read_ansi_string(text_ptr);
        if (result.is_ok()) {
            text = ansi_to_wstring(result.value());
        }
    }

    VITREO_TRACE( "[dialog] SetDlgItemTextA: dlg=0x%x id=%u text_len=%zu\n",
                 dlg, id, text.size());
    bool ok = mgr.set_dlg_item_text(dlg, id, text);
    if (std::getenv("VITREO_WIN_DIAG")) {
        std::string u8 = wstring_to_utf8(text);
        fprintf(stderr, "[settext] SetDlgItemTextA dlg=0x%x id=%u ok=%d text='%s'\n",
                dlg, id, ok ? 1 : 0, u8.c_str());
    }
    m.set_return(ok ? 1u : 0u);
}

static void impl_SetWindowTextA(Marshaler& m, DialogManager& mgr) {
    auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    auto text_ptr = m.arg_ptr(1);

    std::wstring text;
    if (text_ptr != 0) {
        auto result = m.read_ansi_string(text_ptr);
        if (result.is_ok()) {
            // CP_ACP / CP1252 — not UTF-8. A byte such as 0xAE (® in CP1252)
            // is illegal UTF-8; decoding it as UTF-8 replaces it with '?'
            // and can fail a path check that rejects that character.
            text = ansi_to_wstring(result.value());
        }
    }

    VITREO_TRACE( "[dialog] SetWindowTextA: hwnd=0x%x text_len=%zu\n",
                 hwnd, text.size());
    bool ok = mgr.set_window_text(hwnd, text);
    m.set_return(ok ? 1u : 0u);
}

static void impl_GetDlgItemTextA(Marshaler& m, DialogManager& mgr) {
    auto dlg     = static_cast<windowing::HWND>(m.arg(0));
    auto id      = static_cast<u32>(m.arg(1));
    auto buf_ptr = m.arg_ptr(2);
    auto cch_max = static_cast<u32>(m.arg(3));

    std::wstring text = mgr.get_dlg_item_text(dlg, id, cch_max);
    std::string ansi = wstring_to_ansi(text);

    VITREO_TRACE( "[dialog] GetDlgItemTextA: dlg=0x%x id=%u max=%u -> \"%s\"\n",
                 dlg, id, cch_max, wstring_to_utf8(text).c_str());

    if (buf_ptr != 0 && cch_max > 0) {
        (void)m.write_ansi_string(buf_ptr, ansi);
    }

    m.set_return(static_cast<u64>(ansi.size()));
}

static void impl_GetWindowTextA(Marshaler& m, DialogManager& mgr) {
    auto hwnd    = static_cast<windowing::HWND>(m.arg(0));
    auto buf_ptr = m.arg_ptr(1);
    auto cch_max = static_cast<u32>(m.arg(2));
    if (!mgr.is_window(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    if (cch_max == 0) {
        m.set_return(0u);
        return;
    }

    std::wstring text = mgr.get_window_text(hwnd);
    std::string ansi = wstring_to_ansi(text);
    if (cch_max > 0 && ansi.size() > static_cast<std::size_t>(cch_max - 1)) {
        ansi.resize(cch_max - 1);
    }

    VITREO_TRACE( "[dialog] GetWindowTextA: hwnd=0x%x max=%u -> \"%s\"\n",
                 hwnd, cch_max, wstring_to_utf8(text).c_str());

    if (buf_ptr != 0 && cch_max > 0) {
        (void)m.write_ansi_string(buf_ptr, ansi);
    }

    m.set_return(static_cast<u64>(ansi.size()));
}

static void impl_GetWindowTextLengthA(Marshaler& m, DialogManager& mgr) {
    auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    if (!mgr.is_window(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);  // "GetWindowTextLengthW(bad)=0 err=1400"
        m.set_return(0u);
        return;
    }
    std::wstring text = mgr.get_window_text(hwnd);
    std::string ansi = wstring_to_ansi(text);
    m.set_return(static_cast<u64>(ansi.size()));
}

static void impl_GetWindowTextLengthW(Marshaler& m, DialogManager& mgr) {
    auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    if (!mgr.is_window(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    std::wstring text = mgr.get_window_text(hwnd);
    m.set_return(static_cast<u64>(text.size()));
}

// =============================================================================
// Export Registration
// =============================================================================

DllExports create_dialog_thunks(DialogManager& mgr) {
    DllExports exports;
    exports.entries.reserve(19);

    exports.entries.push_back(ExportEntry{
        "CreateDialogParamW", 1,
        [&mgr](Marshaler& m) { impl_CreateDialogParamW(m, mgr); },
        20
    });
    exports.entries.push_back(ExportEntry{
        "CreateDialogParamA", 0,
        [&mgr](Marshaler& m) { impl_CreateDialogParamA(m, mgr); },
        20
    });
    exports.entries.push_back(ExportEntry{
        "DialogBoxParamW", 0,
        [&mgr](Marshaler& m) { impl_DialogBoxParamW(m, mgr); },
        20
    });
    exports.entries.push_back(ExportEntry{
        "DialogBoxParamA", 0,
        [&mgr](Marshaler& m) { impl_DialogBoxParamA(m, mgr); },
        20
    });
    exports.entries.push_back(ExportEntry{
        "EndDialog", 2,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
        impl_EndDialog(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "GetDlgItem", 3,
        [&mgr](Marshaler& m) {
        if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
        impl_GetDlgItem(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "SetDlgItemTextW", 4,
        [&mgr](Marshaler& m) { impl_SetDlgItemTextW(m, mgr); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "SetDlgItemTextA", 0,
        [&mgr](Marshaler& m) { impl_SetDlgItemTextA(m, mgr); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "SetWindowTextW", 5,
        [&mgr](Marshaler& m) { impl_SetWindowTextW(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "SetWindowTextA", 0,
        [&mgr](Marshaler& m) { impl_SetWindowTextA(m, mgr); },
        8
    });
    // Undecorated GetProcAddress("SetWindowText") alias of SetWindowTextA.
    exports.entries.push_back(ExportEntry{
        "SetWindowText", 0,
        [&mgr](Marshaler& m) { impl_SetWindowTextA(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "GetDlgItemTextW", 6,
        [&mgr](Marshaler& m) { impl_GetDlgItemTextW(m, mgr); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "GetDlgItemTextA", 0,
        [&mgr](Marshaler& m) { impl_GetDlgItemTextA(m, mgr); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "GetWindowTextW", 7,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) {  m.set_return(0); return; }
            impl_GetWindowTextW(m, mgr);
            },
        12
    });
    exports.entries.push_back(ExportEntry{
        "InternalGetWindowText", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) {
                set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
                m.set_return(0);
                return;
            }
            impl_InternalGetWindowText(m, mgr);
        },
        12
    });
    exports.entries.push_back(ExportEntry{
        "GetWindowTextA", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) {  m.set_return(0); return; }
            impl_GetWindowTextA(m, mgr);
            },
        12
    });
    exports.entries.push_back(ExportEntry{
        "GetWindowTextLengthA", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
            impl_GetWindowTextLengthA(m, mgr);
            },
        4
    });
    exports.entries.push_back(ExportEntry{
        "GetWindowTextLengthW", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE); m.set_return(0); return; }
            impl_GetWindowTextLengthW(m, mgr);
            },
        4
    });
    // Undecorated GetProcAddress("GetWindowText" / "GetWindowTextLength")
    // aliases of the A exports, same pattern as GetClassName.
    exports.entries.push_back(ExportEntry{
        "GetWindowText", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) { m.set_return(0); return; }
            impl_GetWindowTextA(m, mgr);
        },
        12
    });
    exports.entries.push_back(ExportEntry{
        "GetWindowTextLength", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(0) == 0) {
                set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
                m.set_return(0);
                return;
            }
            impl_GetWindowTextLengthA(m, mgr);
        },
        4
    });
    exports.entries.push_back(ExportEntry{
        "GetDlgItemInt", 0,
        [&mgr](Marshaler& m) { impl_GetDlgItemInt(m, mgr); },
        16
    });

    return exports;
}

} // namespace vitreo::winapi
