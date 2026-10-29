// SPDX-License-Identifier: LGPL-2.1-only

/// @file clipboard.cpp
/// @brief The clipboard, and the viewer chain that watches it.
///
/// Open/Close/Empty/SetClipboardData, plus SetClipboardViewer and
/// ChangeClipboardChain — the pre-WM_CLIPBOARDUPDATE notification scheme where
/// each viewer holds the handle of the next and is responsible for forwarding
/// to it. Removing a link means patching the neighbour that pointed at it,
/// which is why the chain code is longer than the data code.
///
/// Gathered from user32_ext.cpp, where these sat in two runs. Closed in both
/// directions before moving.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/user32/messages/message_send.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "core/memory/guest_memory.hpp"
#include "vitreo/result.hpp"

#include <cstring>
#include <string>
#include <vector>

#include <algorithm>

namespace vitreo::winapi {

u64 User32ExtManager::send_clipboard_viewer_message(
    windowing::HWND hwnd, u32 message, u64 wParam, u64 lParam) {
    if (!m_hwnd_mgr.is_valid(hwnd)) {
        return 0;
    }
    if (m_msgsend_mgr != nullptr) {
        return m_msgsend_mgr->send_message(hwnd, message, wParam, lParam);
    }

    windowing::MSG queued{};
    queued.hwnd = hwnd;
    queued.message = message;
    queued.wParam = wParam;
    queued.lParam = lParam;
    (void)m_queue.post(queued);
    return 0;
}

void User32ExtManager::notify_clipboard_viewer() {
    windowing::HWND viewer = windowing::NULL_HWND;
    {
        std::lock_guard lock(m_clipboard_viewer_mutex);
        if (m_clipboard_viewer_head != windowing::NULL_HWND &&
            !m_hwnd_mgr.is_valid(m_clipboard_viewer_head)) {
            m_clipboard_viewer_head = windowing::NULL_HWND;
        }
        viewer = m_clipboard_viewer_head;
    }
    if (viewer != windowing::NULL_HWND) {
        (void)send_clipboard_viewer_message(
            viewer, windowing::WM::DRAWCLIPBOARD, 0, 0);
    }
}

void User32ExtManager::impl_SetClipboardViewer(Marshaler& m) {
    const auto new_viewer = static_cast<windowing::HWND>(m.arg(0));
    if (!m_hwnd_mgr.is_valid(new_viewer)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        VITREO_TRACE(
            "[user32_ext] SetClipboardViewer(hwnd=0x%x) -> NULL gle=%u\n",
            new_viewer, error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0);
        return;
    }

    windowing::HWND previous = windowing::NULL_HWND;
    {
        std::lock_guard lock(m_clipboard_viewer_mutex);
        if (m_clipboard_viewer_head != windowing::NULL_HWND &&
            !m_hwnd_mgr.is_valid(m_clipboard_viewer_head)) {
            m_clipboard_viewer_head = windowing::NULL_HWND;
        }
        previous = m_clipboard_viewer_head;
        m_clipboard_viewer_head = new_viewer;
    }
    VITREO_TRACE(
        "[user32_ext] SetClipboardViewer(hwnd=0x%x) -> next=0x%x\n",
        new_viewer, previous);
    m.set_return(previous);
}

void User32ExtManager::impl_ChangeClipboardChain(Marshaler& m) {
    const auto remove = static_cast<windowing::HWND>(m.arg(0));
    const auto new_next = static_cast<windowing::HWND>(m.arg(1));
    if (!m_hwnd_mgr.is_valid(remove) ||
        (new_next != windowing::NULL_HWND &&
         !m_hwnd_mgr.is_valid(new_next))) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        VITREO_TRACE(
            "[user32_ext] ChangeClipboardChain(remove=0x%x, next=0x%x) "
            "-> FALSE gle=%u\n",
            remove, new_next, error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0);
        return;
    }

    windowing::HWND head = windowing::NULL_HWND;
    {
        std::lock_guard lock(m_clipboard_viewer_mutex);
        if (m_clipboard_viewer_head != windowing::NULL_HWND &&
            !m_hwnd_mgr.is_valid(m_clipboard_viewer_head)) {
            m_clipboard_viewer_head = windowing::NULL_HWND;
        }
        if (m_clipboard_viewer_head == remove) {
            m_clipboard_viewer_head = new_next;
            VITREO_TRACE(
                "[user32_ext] ChangeClipboardChain(remove=0x%x, next=0x%x) "
                "removed head -> TRUE\n",
                remove, new_next);
            m.set_return(1);
            return;
        }
        head = m_clipboard_viewer_head;
    }

    if (head == windowing::NULL_HWND) {
        VITREO_TRACE(
            "[user32_ext] ChangeClipboardChain(remove=0x%x, next=0x%x) "
            "empty chain -> FALSE\n",
            remove, new_next);
        m.set_return(0);
        return;
    }

    const u64 result = send_clipboard_viewer_message(
        head, windowing::WM::CHANGECBCHAIN, remove, new_next);
    VITREO_TRACE(
        "[user32_ext] ChangeClipboardChain(remove=0x%x, next=0x%x) "
        "notified head=0x%x -> %llu\n",
        remove, new_next, head,
        static_cast<unsigned long long>(result));
    m.set_return(result != 0 ? 1u : 0u);
}

namespace {
constexpr u32 CF_TEXT = 1;
constexpr u32 CF_BITMAP = 2;
constexpr u32 CF_OEMTEXT = 7;
constexpr u32 CF_DIB = 8;
constexpr u32 CF_UNICODETEXT = 13;
constexpr u32 CF_LOCALE = 16;
constexpr u32 CF_DIBV5 = 17;
constexpr u32 WM_DESTROYCLIPBOARD = 0x0307;
constexpr u32 WM_CLIPBOARDUPDATE = 0x031D;

bool is_text_format(u32 format) noexcept {
    return format == CF_TEXT || format == CF_OEMTEXT || format == CF_UNICODETEXT;
}
} // namespace

bool User32ExtManager::clipboard_open_by_caller_locked() const {
    return m_clipboard_open &&
           m_clipboard_open_thread == ThreadManager::get_current_thread_id_static();
}

void User32ExtManager::clipboard_free_rendered_locked() {
    if (m_global_local != nullptr) {
        for (const auto& [format, handle] : m_clipboard_rendered) {
            (void)format;
            (void)m_global_local->global_free(handle);
        }
    }
    m_clipboard_rendered.clear();
}

void User32ExtManager::clipboard_sync_host_locked() {
    // Another application copied: its text replaces this process's contents
    // and advances the sequence number (Learn GetClipboardSequenceNumber:
    // incremented whenever the contents change).
    const i64 host = m_host.clipboard_change_count();
    if (host == m_clipboard_host_seen) return;
    m_clipboard_host_seen = host;
    if (host == m_clipboard_host_count) return;
    if (!m_clipboard_entries.empty() && m_clipboard_host_count >= 0) {
        if (m_global_local != nullptr) {
            for (const ClipboardEntry& entry : m_clipboard_entries) {
                if (entry.handle != 0) (void)m_global_local->global_free(entry.handle);
            }
        }
        m_clipboard_entries.clear();
    }
    m_clipboard_host_count = -1;
    clipboard_free_rendered_locked();
    ++m_clipboard_sequence;
}

std::vector<u32> User32ExtManager::clipboard_formats_locked() {
    // EnumClipboardFormats order (s2msg_input_probe
    // enumformats_after_close: "d 10 1 7" for a CF_UNICODETEXT set): the
    // formats as set, then -- once the clipboard was closed -- the
    // synthesized ones, CF_LOCALE first. Host text counts as a
    // CF_UNICODETEXT another application set.
    std::vector<u32> formats;
    bool text = false;
    if (!m_clipboard_entries.empty()) {
        for (const ClipboardEntry& entry : m_clipboard_entries) {
            formats.push_back(entry.format);
            text = text || is_text_format(entry.format);
        }
    } else if (m_clipboard_host_count < 0 && !m_host.clipboard_get_text().empty()) {
        formats.push_back(CF_UNICODETEXT);
        text = true;
    }
    if (text && m_clipboard_closed_since_set) {
        for (const u32 synth : {CF_LOCALE, CF_TEXT, CF_OEMTEXT, CF_UNICODETEXT}) {
            if (std::find(formats.begin(), formats.end(), synth) == formats.end()) {
                formats.push_back(synth);
            }
        }
    }
    return formats;
}

void User32ExtManager::impl_OpenClipboard(Marshaler& m) {
    // BOOL OpenClipboard(HWND hWndNewOwner): open by another thread -> FALSE /
    // ERROR_ACCESS_DENIED (openclipboard_other_thread); the same thread may
    // open it again (openclipboard_again_same_thread).
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const u32 tid = ThreadManager::get_current_thread_id_static();
    std::lock_guard lock(m_clipboard_mutex);
    if (m_clipboard_open && m_clipboard_open_thread != tid) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        m.set_return(0);
        return;
    }
    clipboard_sync_host_locked();
    m_clipboard_open = true;
    m_clipboard_open_thread = tid;
    m_clipboard_open_hwnd = hwnd;
    VITREO_TRACE("[user32_ext] OpenClipboard(0x%x) -> TRUE\n", hwnd);
    m.set_return(1);
}

void User32ExtManager::impl_CloseClipboard(Marshaler& m) {
    // Not open by the caller: FALSE / ERROR_CLIPBOARD_NOT_OPEN
    // (closeclipboard_not_open / closeclipboard_again). Closing makes the
    // synthesized formats available and advances the sequence number
    // (seq_close_advanced), then notifies viewers and format listeners.
    bool notify = false;
    {
        std::lock_guard lock(m_clipboard_mutex);
        if (!clipboard_open_by_caller_locked()) {
            set_last_error(error_codes::ERROR_CLIPBOARD_NOT_OPEN);
            m.set_return(0);
            return;
        }
        m_clipboard_open = false;
        m_clipboard_open_thread = 0;
        m_clipboard_open_hwnd = windowing::NULL_HWND;
        notify = m_clipboard_changed;
        if (m_clipboard_changed) {
            m_clipboard_closed_since_set = true;
            m_clipboard_changed = false;
            ++m_clipboard_sequence;
        }
    }
    if (notify) {
        notify_clipboard_viewer();
        for (const u32 listener : clipboard_listener_snapshot()) {
            windowing::MSG update{};
            update.hwnd = static_cast<windowing::HWND>(listener);
            update.message = WM_CLIPBOARDUPDATE;
            (void)m_queue.post(update);
        }
    }
    VITREO_TRACE("[user32_ext] CloseClipboard -> TRUE\n");
    m.set_return(1);
}

void User32ExtManager::impl_EmptyClipboard(Marshaler& m) {
    // Not open by the caller: FALSE / ERROR_CLIPBOARD_NOT_OPEN
    // (emptyclipboard_not_open). Frees the data handles, makes the opening
    // window the owner (WM_DESTROYCLIPBOARD to the previous owner) and
    // advances the sequence number (seq_empty_advanced).
    windowing::HWND previous_owner = windowing::NULL_HWND;
    {
        std::lock_guard lock(m_clipboard_mutex);
        if (!clipboard_open_by_caller_locked()) {
            set_last_error(error_codes::ERROR_CLIPBOARD_NOT_OPEN);
            m.set_return(0);
            return;
        }
        if (m_global_local) {
            for (const ClipboardEntry& entry : m_clipboard_entries) {
                if (entry.handle != 0) (void)m_global_local->global_free(entry.handle);
            }
            for (u64 handle : m_clipboard_owned_handles) {
                (void)m_global_local->global_free(handle);
            }
        }
        m_clipboard_entries.clear();
        m_clipboard_owned_handles.clear();
        m_clipboard_data.clear();
        clipboard_free_rendered_locked();
        previous_owner = m_clipboard_owner;
        m_clipboard_owner = m_clipboard_open_hwnd;
        m_host.clipboard_clear();
        m_clipboard_host_count = m_host.clipboard_change_count();
        m_clipboard_host_seen = m_clipboard_host_count;
        m_clipboard_closed_since_set = false;
        m_clipboard_changed = true;
        ++m_clipboard_sequence;
    }
    if (previous_owner != windowing::NULL_HWND && m_hwnd_mgr.is_valid(previous_owner) &&
        m_msgsend_mgr != nullptr) {
        (void)m_msgsend_mgr->send_message(previous_owner, WM_DESTROYCLIPBOARD, 0, 0);
    }
    VITREO_TRACE("[user32_ext] EmptyClipboard -> TRUE\n");
    m.set_return(1);
}

void User32ExtManager::impl_SetClipboardData(Marshaler& m) {
    // HANDLE SetClipboardData(UINT uFormat, HANDLE hMem). Not open by the
    // caller: NULL / ERROR_CLIPBOARD_NOT_OPEN (setclipboarddata_not_open).
    // Success returns hMem and sets the last error to 0
    // (setclipboarddata_unicode err=0); hMem NULL is delayed rendering and
    // returns NULL with the last error untouched
    // (setclipboarddata_second_format). The system owns hMem until
    // EmptyClipboard frees it.
    const u32 uFormat = static_cast<u32>(m.arg(0));
    const u64 hMem = m.cpu_is_64bit() ? m.arg(1) : static_cast<u32>(m.arg(1));
    VITREO_TRACE("[user32_ext] SetClipboardData(format=%u, hMem=0x%llx)\n",
                 uFormat, static_cast<unsigned long long>(hMem));
    std::lock_guard lock(m_clipboard_mutex);
    if (!clipboard_open_by_caller_locked()) {
        set_last_error(error_codes::ERROR_CLIPBOARD_NOT_OPEN);
        m.set_return(0u);
        return;
    }
    if (hMem != 0 && is_text_format(uFormat)) {
        // SetClipboardData receives an HGLOBAL, not the address returned by
        // GlobalLock. Movable allocations deliberately use opaque guest
        // handles. Text reaches the host pasteboard.
        const u64 size = m_global_local ? m_global_local->global_size(hMem) : 0;
        const GuestAddr ptr = size ? m_global_local->global_lock(hMem) : 0;
        if (!ptr) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0u);
            return;
        }
        std::string utf8;
        if (uFormat == CF_UNICODETEXT) {
            auto text = m.read_wide_string(ptr, size / 2);
            if (text.is_ok()) utf8 = text.value();
        } else {
            std::vector<u8> bytes;
            for (u64 i = 0; i < size; ++i) {
                const u8 b = m.read_u8(ptr + i).value_or(0);
                if (b == 0) break;
                bytes.push_back(b);
            }
            std::vector<u16> wide(bytes.size());
            const auto decoded = codepage::multi_byte_to_wide(
                uFormat == CF_OEMTEXT ? codepage::kOemCodePage
                                      : codepage::ansi_code_page(),
                0, bytes.data(), bytes.size(), wide.data(), wide.size());
            if (decoded.ok()) {
                wide.resize(decoded.count);
                utf8 = wstring_to_utf8(std::wstring(wide.begin(), wide.end()));
            }
        }
        (void)m_global_local->global_unlock(hMem);
        if (!m_host.clipboard_set_text(utf8)) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0u);
            return;
        }
        m_clipboard_host_count = m_host.clipboard_change_count();
        m_clipboard_host_seen = m_clipboard_host_count;
    }
    auto existing = std::find_if(m_clipboard_entries.begin(), m_clipboard_entries.end(),
                                 [&](const ClipboardEntry& e) { return e.format == uFormat; });
    if (existing != m_clipboard_entries.end()) {
        if (existing->handle != 0 && existing->handle != hMem && m_global_local) {
            (void)m_global_local->global_free(existing->handle);
        }
        existing->handle = hMem;
    } else {
        m_clipboard_entries.push_back(ClipboardEntry{uFormat, hMem});
    }
    if (m_clipboard_host_count < 0) {
        m_clipboard_host_count = m_host.clipboard_change_count();
        m_clipboard_host_seen = m_clipboard_host_count;
    }
    m_clipboard_closed_since_set = false;
    m_clipboard_changed = true;
    ++m_clipboard_sequence;
    clipboard_free_rendered_locked();
    if (hMem != 0) set_last_error(0);
    m.set_return(hMem); // preserve pointer-sized HANDLE
}

u32 User32ExtManager::intern_clipboard_format(const std::string& name) {
    // Clipboard format names live in the global atom table shared with
    // RegisterWindowMessage.
    return register_window_message(name);
}

u32 User32ExtManager::clipboard_format_name(Marshaler& m, bool wide) {
    const auto format = static_cast<u32>(m.arg(0));
    const GuestAddr buffer = m.arg_ptr(1);
    const auto max_count = static_cast<i32>(static_cast<u32>(m.arg(2)));

    std::string name;
    {
        std::lock_guard lock(m_registered_message_mutex);
        auto found = m_clip_format_names.find(format);
        if (found == m_clip_format_names.end() || buffer == 0) {
            // Predefined formats (CF_TEXT and friends) have no name either
            // (getformatname_cf_text: 0 / ERROR_INVALID_PARAMETER).
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0u);
            return 0;
        }
        if (max_count <= 0) {
            // getformatname_zero_len: 0 / ERROR_INSUFFICIENT_BUFFER.
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0u);
            return 0;
        }
        name = found->second;
    }

    // Windows truncates to the buffer and always NUL-terminates, returning the
    // characters copied excluding the terminator. Registered format names are
    // ASCII in practice, so a byte-wise cut is also a character-wise cut.
    const usize room = static_cast<usize>(max_count) - 1;
    if (name.size() > room) name.resize(room);

    if (wide) {
        if (m.write_wide_string(buffer, name).is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0u);
            return 0;
        }
    } else if (m.write_ansi_string(buffer, name).is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return 0;
    }
    m.set_return(static_cast<u32>(name.size()));
    return static_cast<u32>(name.size());
}

void User32ExtManager::impl_RegisterClipboardFormatW(Marshaler& m) {
    GuestAddr lpszFormat = m.arg_ptr(0);
    auto name = m.read_wide_string(lpszFormat);
    if (name.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    const u32 fmt = intern_clipboard_format(name.value());
    VITREO_TRACE("[u32ext] RegisterClipboardFormatW -> 0x%x\n", fmt);
    m.set_return(fmt);
}

void User32ExtManager::impl_RegisterClipboardFormatA(Marshaler& m) {
    // Microsoft Learn (winuser!RegisterClipboardFormatA): ANSI name → UINT in
    // 0xC000..0xFFFF. Same contract as W; SciLexer registers formats during
    // WM_CREATE and expects stdcall (4) so the following SetWindowLong(hwnd,0,…)
    // keeps a correct stack.
    GuestAddr lpszFormat = m.arg_ptr(0);
    auto name = m.read_ansi_string(lpszFormat);
    if (name.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    const u32 fmt = intern_clipboard_format(name.value());
    VITREO_TRACE("[u32ext] RegisterClipboardFormatA -> 0x%x\n", fmt);
    m.set_return(fmt);
}

void User32ExtManager::impl_IsClipboardFormatAvailable(Marshaler& m) {
    // Needs no open clipboard. Synthesized formats appear only once the
    // setter closed it (isformat_text_synth = 0 while open,
    // isformat_text_after_close = 1).
    const u32 format = static_cast<u32>(m.arg(0));
    std::lock_guard lock(m_clipboard_mutex);
    clipboard_sync_host_locked();
    const auto formats = clipboard_formats_locked();
    const bool available =
        std::find(formats.begin(), formats.end(), format) != formats.end();
    VITREO_TRACE("[u32ext] IsClipboardFormatAvailable fmt=%u -> %s\n",
                 format, available ? "TRUE" : "FALSE");
    m.set_return(available ? 1u : 0u);
}

void User32ExtManager::impl_GetUpdatedClipboardFormats(Marshaler& m) {
    // BOOL GetUpdatedClipboardFormats(PUINT lpuiFormats, UINT cFormats,
    //                                 PUINT pcFormatsOut) (Learn), measured
    // x86 == x64 in clipformats_gesture_probe{32,64}.expected: the clipboard
    // need not be open; the list is EnumClipboardFormats' (synthesized
    // formats only after the clipboard was closed); *pcFormatsOut gets the
    // count; too small a buffer is FALSE / ERROR_INSUFFICIENT_BUFFER with the
    // count stored and the buffer untouched; a NULL pcFormatsOut is FALSE /
    // ERROR_NOACCESS.
    const GuestAddr list = m.arg_ptr(0);
    const u32 capacity = static_cast<u32>(m.arg(1));
    const GuestAddr count_out = m.arg_ptr(2);
    if (count_out == 0) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0u);
        return;
    }
    std::vector<u32> formats;
    {
        std::lock_guard lock(m_clipboard_mutex);
        formats = clipboard_formats_locked();
    }
    const u32 count = static_cast<u32>(formats.size());
    if (m.write_u32(count_out, count).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0u);
        return;
    }
    if (capacity < count || (count != 0 && list == 0)) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0u);
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        if (m.write_u32(list + i * 4, formats[i]).is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0u);
            return;
        }
    }
    m.set_return(1u);
}

void User32ExtManager::impl_EnumClipboardFormats(Marshaler& m) {
    // UINT EnumClipboardFormats(UINT format): the format after @p format
    // (0 = the first). Needs the caller to have the clipboard open
    // (enumformats_closed: 0 / ERROR_CLIPBOARD_NOT_OPEN); the end of the list
    // is 0 with the last error set to 0 (enumformats ... end_err=0).
    const u32 after = static_cast<u32>(m.arg(0));
    std::lock_guard lock(m_clipboard_mutex);
    if (!clipboard_open_by_caller_locked()) {
        set_last_error(error_codes::ERROR_CLIPBOARD_NOT_OPEN);
        m.set_return(0u);
        return;
    }
    const auto formats = clipboard_formats_locked();
    u32 next = 0;
    if (after == 0) {
        next = formats.empty() ? 0u : formats.front();
    } else {
        const auto it = std::find(formats.begin(), formats.end(), after);
        if (it != formats.end() && it + 1 != formats.end()) next = *(it + 1);
    }
    if (next == 0) set_last_error(0);
    VITREO_TRACE("[u32ext] EnumClipboardFormats(%u) -> %u\n", after, next);
    m.set_return(next);
}

std::string User32ExtManager::clipboard_text_utf8_locked(Marshaler& m) {
    // A text format this process set, else the host pasteboard's text.
    if (m_global_local != nullptr) {
        for (const ClipboardEntry& entry : m_clipboard_entries) {
            if (entry.handle == 0 || !is_text_format(entry.format)) continue;
            const u64 size = m_global_local->global_size(entry.handle);
            const GuestAddr ptr = size ? m_global_local->global_lock(entry.handle) : 0;
            if (ptr == 0) continue;
            std::string utf8;
            bool have_text = false;
            if (entry.format == CF_UNICODETEXT) {
                auto text = m.read_wide_string(ptr, size / 2);
                if (text.is_ok()) { utf8 = text.value(); have_text = true; }
            } else {
                std::vector<u8> bytes;
                for (u64 i = 0; i < size; ++i) {
                    const u8 b = m.read_u8(ptr + i).value_or(0);
                    if (b == 0) break;
                    bytes.push_back(b);
                }
                std::vector<u16> wide(bytes.size());
                const auto decoded = codepage::multi_byte_to_wide(
                    entry.format == CF_OEMTEXT ? codepage::kOemCodePage
                                               : codepage::ansi_code_page(),
                    0, bytes.data(), bytes.size(), wide.data(), wide.size());
                if (decoded.ok()) {
                    wide.resize(decoded.count);
                    utf8 = wstring_to_utf8(std::wstring(wide.begin(), wide.end()));
                    have_text = true;
                }
            }
            (void)m_global_local->global_unlock(entry.handle);
            if (have_text) return utf8;
        }
    }
    return m_host.clipboard_get_text();
}

bool User32ExtManager::edit_clipboard_copy(Marshaler& m, windowing::HWND owner,
                                           const std::u16string& text) {
    // The EDIT control's WM_COPY / WM_CUT: OpenClipboard(owner),
    // EmptyClipboard, SetClipboardData(CF_UNICODETEXT), CloseClipboard. A
    // clipboard another thread holds open makes the copy do nothing.
    windowing::HWND previous_owner = windowing::NULL_HWND;
    {
        std::lock_guard lock(m_clipboard_mutex);
        const u32 tid = ThreadManager::get_current_thread_id_static();
        if ((m_clipboard_open && m_clipboard_open_thread != tid) || m_global_local == nullptr) {
            return false;
        }
        clipboard_sync_host_locked();
        for (const ClipboardEntry& entry : m_clipboard_entries) {
            if (entry.handle != 0) (void)m_global_local->global_free(entry.handle);
        }
        for (u64 handle : m_clipboard_owned_handles) (void)m_global_local->global_free(handle);
        m_clipboard_entries.clear();
        m_clipboard_owned_handles.clear();
        m_clipboard_data.clear();
        clipboard_free_rendered_locked();
        previous_owner = m_clipboard_owner;
        m_clipboard_owner = owner;
        ++m_clipboard_sequence;  // EmptyClipboard

        constexpr u32 kGhnd = 0x0042u;  // GMEM_MOVEABLE | GMEM_ZEROINIT
        const u64 h = m_global_local->global_alloc(kGhnd, (text.size() + 1) * 2);
        const GuestAddr ptr = h != 0 ? m_global_local->global_lock(h) : 0;
        if (ptr != 0) {
            for (usize i = 0; i < text.size(); ++i) {
                (void)m_memory.write_u16(ptr + i * 2, static_cast<u16>(text[i]));
            }
            (void)m_memory.write_u16(ptr + text.size() * 2, 0);
            (void)m_global_local->global_unlock(h);
            m_clipboard_entries.push_back(ClipboardEntry{CF_UNICODETEXT, h});
            std::wstring wide;
            for (usize i = 0; i < text.size(); ++i) {
                const char16_t c = text[i];
                if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.size()) {
                    wide.push_back(static_cast<wchar_t>(
                        0x10000 + ((c - 0xD800) << 10) + (text[i + 1] - 0xDC00)));
                    ++i;
                } else {
                    wide.push_back(static_cast<wchar_t>(c));
                }
            }
            (void)m_host.clipboard_set_text(wstring_to_utf8(wide));
            m_clipboard_host_count = m_host.clipboard_change_count();
            m_clipboard_host_seen = m_clipboard_host_count;
            ++m_clipboard_sequence;  // SetClipboardData
        } else if (h != 0) {
            (void)m_global_local->global_free(h);
        }
        m_clipboard_closed_since_set = true;
        m_clipboard_changed = false;
        ++m_clipboard_sequence;  // CloseClipboard after a change
    }
    if (previous_owner != windowing::NULL_HWND && previous_owner != owner &&
        m_hwnd_mgr.is_valid(previous_owner) && m_msgsend_mgr != nullptr) {
        (void)m_msgsend_mgr->send_message(previous_owner, WM_DESTROYCLIPBOARD, 0, 0);
    }
    notify_clipboard_viewer();
    for (const u32 listener : clipboard_listener_snapshot()) {
        windowing::MSG update{};
        update.hwnd = static_cast<windowing::HWND>(listener);
        update.message = WM_CLIPBOARDUPDATE;
        (void)m_queue.post(update);
    }
    return true;
}

std::optional<std::u16string> User32ExtManager::edit_clipboard_text(Marshaler& m) {
    // The EDIT control's WM_PASTE: CF_UNICODETEXT (or a synthesized text
    // format) if the clipboard can be opened and holds text.
    std::lock_guard lock(m_clipboard_mutex);
    const u32 tid = ThreadManager::get_current_thread_id_static();
    if (m_clipboard_open && m_clipboard_open_thread != tid) return std::nullopt;
    clipboard_sync_host_locked();
    const auto formats = clipboard_formats_locked();
    const bool has_text = std::any_of(formats.begin(), formats.end(),
                                      [](u32 f) { return is_text_format(f); });
    if (!has_text) return std::nullopt;
    const std::wstring wide = utf8_to_wstring(clipboard_text_utf8_locked(m));
    std::u16string out;
    for (wchar_t wc : wide) {
        const char32_t cp = static_cast<char32_t>(wc);
        if (cp < 0x10000) {
            out.push_back(static_cast<char16_t>(cp));
        } else {
            out.push_back(static_cast<char16_t>(0xD800 + ((cp - 0x10000) >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + ((cp - 0x10000) & 0x3FF)));
        }
    }
    return out;
}

void User32ExtManager::impl_GetClipboardData(Marshaler& m) {
    // HANDLE GetClipboardData(UINT uFormat). Needs the caller to have the
    // clipboard open (getclipboarddata_not_open: NULL /
    // ERROR_CLIPBOARD_NOT_OPEN); a format that is not there is NULL /
    // ERROR_NOT_FOUND (getclipboarddata_absent). A format that was set comes
    // back as the handle that was set; a synthesized one, or host text, is
    // rendered into a system-owned HGLOBAL.
    const u32 fmt = static_cast<u32>(m.arg(0));
    std::lock_guard lock(m_clipboard_mutex);
    if (!clipboard_open_by_caller_locked()) {
        set_last_error(error_codes::ERROR_CLIPBOARD_NOT_OPEN);
        m.set_return(0u);
        return;
    }
    const auto formats = clipboard_formats_locked();
    if (std::find(formats.begin(), formats.end(), fmt) == formats.end()) {
        set_last_error(error_codes::ERROR_NOT_FOUND);
        m.set_return(0u);
        return;
    }
    for (const ClipboardEntry& entry : m_clipboard_entries) {
        if (entry.format == fmt && entry.handle != 0) {
            m.set_return(entry.handle);
            return;
        }
    }
    if (const auto it = m_clipboard_rendered.find(fmt); it != m_clipboard_rendered.end()) {
        m.set_return(it->second);
        return;
    }
    if (m_global_local == nullptr) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        m.set_return(0u);
        return;
    }
    // Render from the text this clipboard holds: a text format this process
    // set, else the host pasteboard.
    const std::string utf8 = clipboard_text_utf8_locked(m);

    std::vector<u8> bytes;
    if (fmt == CF_LOCALE) {
        // The LCID of the text: en-US.
        constexpr u32 kLcid = 0x0409;
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<u8>(kLcid >> (8 * i)));
    } else {
        const std::wstring wide_host = utf8_to_wstring(utf8);
        std::vector<u16> wide;
        for (wchar_t wc : wide_host) {
            const char32_t cp = static_cast<char32_t>(wc);
            if (cp < 0x10000) {
                wide.push_back(static_cast<u16>(cp));
            } else {
                const char32_t adj = cp - 0x10000;
                wide.push_back(static_cast<u16>(0xD800 + (adj >> 10)));
                wide.push_back(static_cast<u16>(0xDC00 + (adj & 0x3FF)));
            }
        }
        if (fmt == CF_UNICODETEXT) {
            for (u16 cu : wide) {
                bytes.push_back(static_cast<u8>(cu & 0xFF));
                bytes.push_back(static_cast<u8>(cu >> 8));
            }
            bytes.push_back(0);
            bytes.push_back(0);
        } else {
            std::vector<u8> narrow(wide.size() * 2 + 1);
            const auto encoded = codepage::wide_to_multi_byte(
                fmt == CF_OEMTEXT ? codepage::kOemCodePage : codepage::ansi_code_page(),
                0, wide.data(), wide.size(), narrow.data(), narrow.size(), nullptr,
                false);
            narrow.resize(encoded.ok() ? encoded.count : 0);
            bytes = std::move(narrow);
            bytes.push_back(0);
        }
    }
    // GMEM_MOVEABLE | GMEM_ZEROINIT — classic clipboard HGLOBAL
    constexpr u32 kGhnd = 0x0042u;
    const u64 h = m_global_local->global_alloc(kGhnd, bytes.size());
    const GuestAddr ptr = h != 0 ? m_global_local->global_lock(h) : 0;
    if (ptr == 0) {
        if (h != 0) (void)m_global_local->global_free(h);
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        m.set_return(0u);
        return;
    }
    (void)m_memory.write_bytes(ptr, bytes.data(), bytes.size());
    (void)m_global_local->global_unlock(h);
    m_clipboard_rendered[fmt] = h;
    VITREO_TRACE("[u32ext] GetClipboardData fmt=%u -> HGLOBAL=0x%llx (%zu bytes)\n",
                 fmt, static_cast<unsigned long long>(h), bytes.size());
    m.set_return(h);
}

void User32ExtManager::impl_GetClipboardFormatNameA(Marshaler& m) {
    // int GetClipboardFormatNameA(UINT format, LPSTR lpszFormatName, int cchMax)
    const u32 copied = clipboard_format_name(m, /*wide=*/false);
    VITREO_TRACE("[u32ext] GetClipboardFormatNameA -> %u\n", copied);
}

void User32ExtManager::impl_GetClipboardFormatNameW(Marshaler& m) {
    // int GetClipboardFormatNameW(UINT format, LPWSTR lpszFormatName, int cchMax)
    const u32 copied = clipboard_format_name(m, /*wide=*/true);
    VITREO_TRACE("[u32ext] GetClipboardFormatNameW -> %u\n", copied);
}

void User32ExtManager::impl_GetClipboardSequenceNumber(Marshaler& m) {
    // Learn: incremented whenever the clipboard contents change or it is
    // emptied (seq_empty_advanced, seq_set_advanced, seq_close_advanced) --
    // including a copy in another application.
    std::lock_guard lock(m_clipboard_mutex);
    clipboard_sync_host_locked();
    VITREO_TRACE("[u32ext] GetClipboardSequenceNumber -> %u\n", m_clipboard_sequence);
    m.set_return(m_clipboard_sequence);
}

} // namespace vitreo::winapi
