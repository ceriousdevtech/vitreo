// SPDX-License-Identifier: LGPL-2.1-only
/// @file control_edit.cpp
/// @brief EDIT class procedure: the Windows-visible text model and every
/// EM_* message, plus WM_SETTEXT / WM_GETTEXT / WM_GETTEXTLENGTH and
/// WM_CUT / WM_COPY / WM_PASTE / WM_CLEAR / WM_UNDO.
///
/// Sources: Learn "Edit Control Reference" (each EM_* / EN_* page);
/// constants from SDK WinUser.h / CommCtrl.h 10.0.26100.0. Behaviour Learn
/// leaves unstated is measured on Windows (comctl32 v6, x86 and x64):
/// tools/oracle_probe/edit_messages_probe.c and edit_messages2_probe.c ->
/// *.expected. Line references below name those files' lines.
///
/// The control owns the text with Windows line breaks (CRLF); the native
/// view shows a display copy (CRLF as one newline) and is where the user
/// types. Every message first folds user edits back into the model.

#include "winapi/user32/messages/message_send.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/user32/messages/sdk_edit_readonly.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/user32/ansi_text.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::winapi {

namespace {

// WinUser.h (EM_LIMITTEXT / EM_GETLIMITTEXT come from message_send.hpp)
constexpr u32 EM_GETSEL = 0x00B0, EM_SETSEL = 0x00B1, EM_GETRECT = 0x00B2,
              EM_SETRECT = 0x00B3, EM_SETRECTNP = 0x00B4, EM_SCROLL = 0x00B5,
              EM_LINESCROLL = 0x00B6, EM_SCROLLCARET = 0x00B7, EM_GETMODIFY = 0x00B8,
              EM_SETMODIFY = 0x00B9, EM_GETLINECOUNT = 0x00BA, EM_LINEINDEX = 0x00BB,
              EM_SETHANDLE = 0x00BC, EM_GETHANDLE = 0x00BD, EM_GETTHUMB = 0x00BE,
              EM_LINELENGTH = 0x00C1, EM_REPLACESEL = 0x00C2, EM_GETLINE = 0x00C4,
              EM_CANUNDO = 0x00C6, EM_UNDO = 0x00C7,
              EM_FMTLINES = 0x00C8, EM_LINEFROMCHAR = 0x00C9, EM_SETTABSTOPS = 0x00CB,
              EM_SETPASSWORDCHAR = 0x00CC, EM_EMPTYUNDOBUFFER = 0x00CD,
              EM_GETFIRSTVISIBLELINE = 0x00CE, EM_SETREADONLY = sdk_edit_readonly::EM_SETREADONLY,
              EM_SETWORDBREAKPROC = 0x00D0, EM_GETWORDBREAKPROC = 0x00D1,
              EM_GETPASSWORDCHAR = 0x00D2, EM_SETMARGINS = 0x00D3, EM_GETMARGINS = 0x00D4,
              EM_POSFROMCHAR = 0x00D6, EM_CHARFROMPOS = 0x00D7,
              EM_SETIMESTATUS = 0x00D8, EM_GETIMESTATUS = 0x00D9, EM_ENABLEFEATURE = 0x00DA;
constexpr u32 EN_CHANGE = 0x0300, EN_UPDATE = 0x0400, EN_MAXTEXT = 0x0501;
constexpr u32 EC_LEFTMARGIN = 0x0001, EC_RIGHTMARGIN = 0x0002, EC_USEFONTINFO = 0xFFFF;
constexpr u32 SB_LINEUP = 0, SB_LINEDOWN = 1, SB_PAGEUP = 2, SB_PAGEDOWN = 3;
// CommCtrl.h (ECM_FIRST = 0x1500)
constexpr u32 EM_SETCUEBANNER = 0x1501, EM_GETCUEBANNER = 0x1502,
              EM_SHOWBALLOONTIP = 0x1503, EM_HIDEBALLOONTIP = 0x1504,
              EM_SETHILITE = 0x1505, EM_GETHILITE = 0x1506, EM_NOSETFOCUS = 0x1507,
              EM_TAKEFOCUS = 0x1508, EM_SETEXTENDEDSTYLE = 0x150A,
              EM_GETEXTENDEDSTYLE = 0x150B, EM_SETENDOFLINE = 0x150C,
              EM_GETENDOFLINE = 0x150D, EM_ENABLESEARCHWEB = 0x150E, EM_SEARCHWEB = 0x150F,
              EM_SETCARETINDEX = 0x1511, EM_GETCARETINDEX = 0x1512,
              EM_FILELINEFROMCHAR = 0x1513, EM_FILELINEINDEX = 0x1514,
              EM_FILELINELENGTH = 0x1515, EM_GETFILELINE = 0x1516,
              EM_GETFILELINECOUNT = 0x1517, EM_GETZOOM = 0x04E0, EM_SETZOOM = 0x04E1;
constexpr u32 ES_EX_ALLOWEOL_CR = 0x1, ES_EX_ALLOWEOL_LF = 0x2, ES_EX_ZOOMABLE = 0x10;
// Window messages
constexpr u32 WM_SETTEXT_ = 0x000C, WM_GETTEXT_ = 0x000D, WM_GETTEXTLENGTH_ = 0x000E,
              WM_CUT_ = 0x0300, WM_COPY_ = 0x0301, WM_PASTE_ = 0x0302, WM_CLEAR_ = 0x0303,
              WM_UNDO_ = 0x0304, WM_COMMAND_ = 0x0111;
// Edit styles
constexpr u32 ES_MULTILINE = 0x0004, ES_UPPERCASE = 0x0008, ES_LOWERCASE = 0x0010,
              ES_PASSWORD = 0x0020, ES_AUTOHSCROLL = 0x0080, ES_READONLY = sdk_edit_readonly::ES_READONLY,
              WS_HSCROLL_ = 0x00100000;
/// Default ES_PASSWORD character in comctl32 v6 (edit_messages2 "pwchar=0x25cf").
constexpr u16 kDefaultPasswordChar = 0x25CF;
/// Shown in place of a CR or LF that is not a line break.
constexpr char16_t kLoneBreakGlyph = u'▯';

void unimplemented_once(const char* what) {
    static std::mutex mutex;
    static std::vector<std::string> seen;
    std::lock_guard<std::mutex> lock(mutex);
    if (std::find(seen.begin(), seen.end(), what) != seen.end()) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!EDIT %s\n", what);
}

[[nodiscard]] bool ieq(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (std::towlower(static_cast<wint_t>(a[i])) != std::towlower(static_cast<wint_t>(b[i]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::u16string to_u16(const std::wstring& w) {
    std::u16string out;
    for (wchar_t wc : w) {
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

[[nodiscard]] std::wstring to_w(const std::u16string& s) {
    std::wstring out;
    for (usize i = 0; i < s.size(); ++i) {
        const char16_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            out.push_back(static_cast<wchar_t>(0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00)));
            ++i;
        } else {
            out.push_back(static_cast<wchar_t>(c));
        }
    }
    return out;
}

[[nodiscard]] std::string to_acp(const std::u16string& s) { return ansi_text::to_ansi(to_w(s)); }
[[nodiscard]] std::u16string from_acp(const std::string& s) { return to_u16(ansi_text::to_wide(s)); }

struct Line {
    u32 start;
    u32 length;  ///< excluding the line break
};

/// Display string plus index maps between model and display (UTF-16 units).
struct Display {
    std::u16string text;
    std::vector<u32> to_display;  ///< model index -> display index (size len+1)
    std::vector<u32> to_model;    ///< display index -> model index (size dlen+1)
};

}  // namespace

// The EDIT procedure's private helpers operate on one snapshot of the state.
namespace {

struct EditContext {
    MessageSendManager::EditState& st;
    u32 style;
    bool multiline() const { return (style & ES_MULTILINE) != 0; }
    bool wraps() const {
        return multiline() && !(style & ES_AUTOHSCROLL) && !(style & WS_HSCROLL_);
    }
    /// Length of the line break starting at @p i (0 if none).
    u32 break_at(usize i) const {
        const auto& t = st.text;
        if (!multiline() || i >= t.size()) return 0;
        if (t[i] == u'\r' && i + 1 < t.size() && t[i + 1] == u'\n') return 2;
        if (t[i] == u'\r' && (st.ex_style & ES_EX_ALLOWEOL_CR)) return 1;
        if (t[i] == u'\n' && (st.ex_style & ES_EX_ALLOWEOL_LF)) return 1;
        return 0;
    }
    /// Lines split at line breaks only ("file lines").
    std::vector<Line> hard_lines() const {
        std::vector<Line> lines;
        u32 start = 0;
        for (usize i = 0; i < st.text.size();) {
            const u32 br = break_at(i);
            if (br != 0) {
                lines.push_back({start, static_cast<u32>(i) - start});
                i += br;
                start = static_cast<u32>(i);
            } else {
                ++i;
            }
        }
        lines.push_back({start, static_cast<u32>(st.text.size()) - start});
        return lines;
    }
    Display display() const {
        Display d;
        const auto& t = st.text;
        d.to_display.assign(t.size() + 1, 0);
        for (usize i = 0; i < t.size();) {
            d.to_display[i] = static_cast<u32>(d.text.size());
            const u32 br = break_at(i);
            if (br == 2) {
                d.to_display[i + 1] = static_cast<u32>(d.text.size());
                d.text.push_back(u'\n');
                i += 2;
                continue;
            }
            if (br == 1) {
                d.text.push_back(u'\n');
            } else if (t[i] == u'\r' || t[i] == u'\n') {
                d.text.push_back(kLoneBreakGlyph);
            } else {
                d.text.push_back(t[i]);
            }
            ++i;
        }
        d.to_display[t.size()] = static_cast<u32>(d.text.size());
        d.to_model.assign(d.text.size() + 1, static_cast<u32>(t.size()));
        for (usize i = t.size() + 1; i-- > 0;) d.to_model[d.to_display[i]] = static_cast<u32>(i);
        return d;
    }
};

[[nodiscard]] std::u16string model_from_display(const std::u16string& shown, bool multiline) {
    if (!multiline) return shown;
    std::u16string out;
    for (char16_t c : shown) {
        if (c == u'\n') { out.push_back(u'\r'); out.push_back(u'\n'); }
        else out.push_back(c);
    }
    return out;
}

}  // namespace

bool MessageSendManager::dispatch_edit_control(Marshaler& m, windowing::HWND hwnd, u32 msg,
                                               u64 wParam, u64 lParam, bool ansi,
                                               bool force_native) {
    windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (win == nullptr) return false;
    const auto& class_name = win->control_class_name();
    const bool edit = ieq(class_name, L"Edit");
    const bool rich = ieq(class_name, L"RichEdit") || ieq(class_name, L"RichEdit20A") ||
                      ieq(class_name, L"RichEdit20W") || ieq(class_name, L"RichEdit50W");
    if (!edit && !(rich && msg == EM_SETREADONLY)) return false;
    if (!force_native && win->wndproc != 0) return false;
    if (msg == EM_SETREADONLY) {
        // Shared documented message; preserve text and selection rather than
        // initializing/recommitting the plain EDIT text model for Rich Edit.
        // Windows edit_readonly_probe{32,64}: Rich Edit consumes a BOOL-sized
        // input and clears LastError on a style transition; EDIT uses WPARAM.
        const bool read_only = rich ? static_cast<u32>(wParam) != 0 : wParam != 0;
        const bool changed = ((win->style & ES_READONLY) != 0) != read_only;
        if (read_only) win->style |= ES_READONLY;
        else win->style &= ~ES_READONLY;
        m_bridge.edit_set_read_only(hwnd, read_only);
        if (rich && changed) set_last_error(0);
        m.set_return(1u);
        return true;
    }
    const bool is_em = (msg >= EM_GETSEL && msg <= EM_ENABLEFEATURE) ||
                       (msg >= EM_SETCUEBANNER && msg <= EM_GETFILELINECOUNT) ||
                       msg == EM_GETZOOM || msg == EM_SETZOOM;
    const bool is_wm = msg == WM_SETTEXT_ || msg == WM_GETTEXT_ || msg == WM_GETTEXTLENGTH_ ||
                       (msg >= WM_CUT_ && msg <= WM_UNDO_);
    if (!is_em && !is_wm) return false;

    const windowing::HWND parent = win->parent;
    const u32 ctrl_id = win->ctrl_id;
    EditState st;
    {
        std::lock_guard<std::mutex> lock(m_control_state_mutex);
        auto it = m_edit_state.find(hwnd);
        if (it != m_edit_state.end()) st = it->second;
    }
    const bool first_use = !st.initialized;
    if (first_use) {
        st.initialized = true;
        st.text = to_u16(win->title);
        st.end_of_line = (win->style & ES_MULTILINE) ? 1u : 0u;  // edit_messages2 "eol="
        if (win->style & ES_PASSWORD) st.password_char = kDefaultPasswordChar;
    }
    EditContext cx{st, win->style};
    const bool multi = cx.multiline();

    if (first_use) {
        // The model starts from the window text and the view shows its
        // display form (CRLF as one newline) -- unless the view already
        // holds other text (typed before the first message), which wins.
        const Display d = cx.display();
        const std::u16string live = to_u16(m_bridge.get_control_text(hwnd));
        if (!live.empty() && live != d.text) {
            st.text = model_from_display(live, multi);
            st.shown = live;
            win->title = to_w(st.text);
        } else {
            st.shown = d.text;
            if (live != d.text) (void)m_bridge.set_control_text(hwnd, to_w(d.text));
        }
    } else {
        // Anything the view shows that the model did not put there is the
        // user's typing: fold it back in.
        const std::u16string live = to_u16(m_bridge.get_control_text(hwnd));
        if (live != st.shown) {
            st.undo_text = st.text;
            st.undo_start = 0;
            st.undo_len = static_cast<u32>(st.text.size());
            st.text = model_from_display(live, multi);
            st.redo_len = static_cast<u32>(st.text.size());
            st.can_undo = true;
            st.modified = true;
            st.shown = live;
            if (win != nullptr) win->title = to_w(st.text);
        }
        u32 a = 0, b = 0;
        if (m_bridge.edit_get_selection(hwnd, a, b)) {
            const Display d = cx.display();
            a = d.to_model[std::min<usize>(a, d.to_model.size() - 1)];
            b = d.to_model[std::min<usize>(b, d.to_model.size() - 1)];
            if (std::min(st.anchor, st.caret) != a || std::max(st.anchor, st.caret) != b) {
                st.anchor = a;
                st.caret = b;
            }
        }
    }

    const u32 len = static_cast<u32>(st.text.size());
    auto sel_start = [&] { return std::min(st.anchor, st.caret); };
    auto sel_end = [&] { return std::max(st.anchor, st.caret); };
    auto set_sel = [&](u32 anchor, u32 caret) {
        st.anchor = std::min(anchor, len);
        st.caret = std::min(caret, len);
    };

    // Windows lines (visual lines when the control wraps).
    auto lines = [&]() -> std::vector<Line> {
        if (!multi) return {{0, static_cast<u32>(st.text.size())}};
        if (!cx.wraps()) return cx.hard_lines();
        const Display d = cx.display();
        std::vector<u32> starts;
        for (u32 s : m_bridge.edit_visual_line_starts(hwnd)) {
            starts.push_back(d.to_model[std::min<usize>(s, d.to_model.size() - 1)]);
        }
        if (starts.empty()) return cx.hard_lines();
        std::vector<Line> out;
        for (usize i = 0; i < starts.size(); ++i) {
            const u32 s = starts[i];
            u32 e = i + 1 < starts.size() ? starts[i + 1] : static_cast<u32>(st.text.size());
            // Drop a line break that ends this visual line.
            for (u32 k = s; k < e; ++k) {
                const u32 br = cx.break_at(k);
                if (br != 0 && k + br == e) { e = k; break; }
            }
            out.push_back({s, e - s});
        }
        return out;
    };
    auto line_of = [&](const std::vector<Line>& ls, u32 index) -> u32 {
        for (usize i = ls.size(); i-- > 0;) {
            if (index >= ls[i].start) return static_cast<u32>(i);
        }
        return 0;
    };

    auto notify = [&](u32 code) {
        if (parent == windowing::NULL_HWND) return;
        (void)send_message(parent, WM_COMMAND_,
                           (static_cast<u64>(code) << 16) | (ctrl_id & 0xFFFFu), hwnd);
    };
    auto push_view = [&] {
        const Display d = cx.display();
        st.shown = d.text;
        (void)m_bridge.set_control_text(hwnd, to_w(d.text));
        win = m_hwnd_mgr.find(hwnd);
        if (win != nullptr) win->title = to_w(st.text);
        (void)m_bridge.edit_set_selection(hwnd, d.to_display[std::min(st.anchor, len)],
                                          d.to_display[std::min(st.caret, len)]);
    };
    auto push_selection = [&] {
        const Display d = cx.display();
        (void)m_bridge.edit_set_selection(hwnd, d.to_display[std::min(st.anchor, static_cast<u32>(st.text.size()))],
                                          d.to_display[std::min(st.caret, static_cast<u32>(st.text.size()))]);
    };
    auto commit = [&] {
        std::lock_guard<std::mutex> lock(m_control_state_mutex);
        m_edit_state[hwnd] = st;
    };
    auto case_fold = [&](std::u16string s) {
        if (cx.style & ES_UPPERCASE) {
            for (auto& c : s) c = static_cast<char16_t>(std::towupper(static_cast<wint_t>(c)));
        } else if (cx.style & ES_LOWERCASE) {
            for (auto& c : s) c = static_cast<char16_t>(std::towlower(static_cast<wint_t>(c)));
        }
        return s;
    };
    auto read_text_arg = [&](GuestAddr at) -> std::u16string {
        if (at == 0) return {};
        if (ansi) {
            std::string bytes;
            for (u32 off = 0; off < 0x7FFFFFFF; ++off) {
                auto c = m.read_u8(at + off);
                if (!c.is_ok() || c.value() == 0) break;
                bytes.push_back(static_cast<char>(c.value()));
            }
            return from_acp(bytes);
        }
        std::u16string out;
        for (u32 off = 0; off < 0x7FFFFFFF; ++off) {
            auto c = m.read_u16(at + off * 2);
            if (!c.is_ok() || c.value() == 0) break;
            out.push_back(static_cast<char16_t>(c.value()));
        }
        return out;
    };
    /// Copy @p s into a buffer of @p capacity units, no terminator; returns
    /// the units copied (EM_GETLINE protocol).
    auto copy_no_nul = [&](GuestAddr buf, const std::u16string& s, u32 capacity) -> u32 {
        if (ansi) {
            const std::string bytes = to_acp(s);
            const u32 n = std::min<u32>(capacity, static_cast<u32>(bytes.size()));
            for (u32 i = 0; i < n; ++i) (void)m.write_u8(buf + i, static_cast<u8>(bytes[i]));
            return n;
        }
        const u32 n = std::min<u32>(capacity, static_cast<u32>(s.size()));
        for (u32 i = 0; i < n; ++i) (void)m.write_u16(buf + i * 2, static_cast<u16>(s[i]));
        return n;
    };

    /// Replace the selection with @p insert (EM_REPLACESEL, WM_CLEAR,
    /// WM_CUT, WM_PASTE): EM_LIMITTEXT truncates the insert with EN_MAXTEXT;
    /// EN_UPDATE / EN_CHANGE once (single-line) or per delete and insert
    /// step (multiline); EN_MAXTEXT before them single-line, after them
    /// multiline (edit_messages2 "replacesel", "maxtext").
    auto replace_selection = [&](std::u16string insert, bool can_undo) {
        insert = case_fold(std::move(insert));
        const u32 a = sel_start();
        const u32 b = sel_end();
        const u32 removed = b - a;
        const u64 room = st.limit >= (len - removed) ? st.limit - (len - removed) : 0;
        bool truncated = false;
        if (insert.size() > room) {
            insert.resize(static_cast<usize>(room));
            truncated = true;
        }
        const bool changes = removed != 0 || !insert.empty();
        if (changes) {
            if (can_undo) {
                st.undo_text = st.text;
                st.undo_start = a;
                st.undo_len = removed;
                st.redo_len = static_cast<u32>(insert.size());
                st.can_undo = true;
            } else {
                st.can_undo = false;
                st.undo_text.clear();
            }
            st.text.replace(a, removed, insert);
            const u32 caret = a + static_cast<u32>(insert.size());
            st.anchor = caret;
            st.caret = caret;
            st.modified = true;
            push_view();
        }
        commit();
        if (truncated && !multi) notify(EN_MAXTEXT);
        if (changes) {
            const int steps = multi ? (removed != 0) + (!insert.empty()) : 1;
            for (int i = 0; i < steps; ++i) { notify(EN_UPDATE); notify(EN_CHANGE); }
        }
        if (truncated && multi) notify(EN_MAXTEXT);
    };

    switch (msg) {
    // ---------------------------------------------------------------- text
    case WM_SETTEXT_: {
        // Not limited by EM_LIMITTEXT (edit_messages "settext_over_limit");
        // resets modify and undo; single-line sends EN_UPDATE + EN_CHANGE,
        // multiline nothing (edit_messages2 "settext notify").
        st.text = case_fold(read_text_arg(static_cast<GuestAddr>(lParam)));
        st.modified = false;
        st.can_undo = false;
        st.undo_text.clear();
        st.anchor = st.caret = 0;
        push_view();
        commit();
        if (!multi) { notify(EN_UPDATE); notify(EN_CHANGE); }
        m.set_return(1u);
        return true;
    }
    case WM_GETTEXTLENGTH_: {
        commit();
        m.set_return(ansi ? to_acp(st.text).size() : st.text.size());
        return true;
    }
    case WM_GETTEXT_: {
        commit();
        // EM_FMTLINES adds CR CR LF at each soft break of a wrapping control.
        std::u16string text = st.text;
        if (st.fmtlines && cx.wraps()) {
            std::u16string out;
            const auto ls = lines();
            for (usize i = 0; i < ls.size(); ++i) {
                const u32 end = ls[i].start + ls[i].length;
                out += st.text.substr(ls[i].start, ls[i].length);
                if (i + 1 < ls.size()) {
                    if (cx.break_at(end) != 0) out += st.text.substr(end, ls[i + 1].start - end);
                    else out += u"\r\r\n";
                }
            }
            text = out;
        }
        const u32 cch = static_cast<u32>(wParam);
        const GuestAddr buf = static_cast<GuestAddr>(lParam);
        if (cch == 0 || buf == 0) { m.set_return(0u); return true; }
        if (ansi) {
            const std::string bytes = to_acp(text);
            const u32 n = std::min<u32>(cch - 1, static_cast<u32>(bytes.size()));
            for (u32 i = 0; i < n; ++i) (void)m.write_u8(buf + i, static_cast<u8>(bytes[i]));
            (void)m.write_u8(buf + n, 0);
            m.set_return(n);
        } else {
            const u32 n = std::min<u32>(cch - 1, static_cast<u32>(text.size()));
            for (u32 i = 0; i < n; ++i) (void)m.write_u16(buf + i * 2, static_cast<u16>(text[i]));
            (void)m.write_u16(buf + n * 2, 0);
            m.set_return(n);
        }
        return true;
    }
    case EM_REPLACESEL:
        replace_selection(read_text_arg(static_cast<GuestAddr>(lParam)), wParam != 0);
        m.set_return(0u);
        return true;
    case WM_CLEAR_:
        // Measured: WM_CLEAR returns 0 and is undoable (edit_messages2 "wm_clear").
        if (!(cx.style & ES_READONLY) && sel_end() > sel_start()) replace_selection({}, true);
        else commit();
        m.set_return(0u);
        return true;
    case WM_COPY_:
    case WM_CUT_: {
        // A password edit never copies its text; a read-only one copies but
        // does not cut.
        const bool can_copy = st.password_char == 0 && sel_end() > sel_start();
        if (can_copy && m_edit_clipboard_copy) {
            (void)m_edit_clipboard_copy(m, hwnd, st.text.substr(sel_start(), sel_end() - sel_start()));
        }
        if (msg == WM_CUT_ && can_copy && !(cx.style & ES_READONLY)) replace_selection({}, true);
        else commit();
        m.set_return(0u);
        return true;
    }
    case WM_PASTE_: {
        std::optional<std::u16string> clip;
        if (!(cx.style & ES_READONLY) && m_edit_clipboard_text) clip = m_edit_clipboard_text(m);
        if (clip) {
            std::u16string text = *clip;
            if (!multi) {
                // A single-line edit takes the first line.
                const auto br = text.find_first_of(u"\r\n");
                if (br != std::u16string::npos) text.resize(br);
            }
            replace_selection(text, true);
        } else {
            commit();
        }
        m.set_return(0u);
        return true;
    }
    case EM_UNDO:
    case WM_UNDO_: {
        // Undo and redo alternate; the restored range is left selected
        // (edit_messages "after_undo"). Password edits undo differently on
        // Windows (edit_messages2 "password wm_undo"); not modelled.
        if (!st.can_undo) {
            commit();
            m.set_return(multi ? 0u : 1u);
            return true;
        }
        std::swap(st.text, st.undo_text);
        std::swap(st.undo_len, st.redo_len);
        st.anchor = st.undo_start;
        st.caret = st.undo_start + st.redo_len;
        if (st.caret > st.text.size()) st.caret = static_cast<u32>(st.text.size());
        st.modified = true;
        push_view();
        commit();
        const int steps = multi ? 2 : 1;
        for (int i = 0; i < steps; ++i) { notify(EN_UPDATE); notify(EN_CHANGE); }
        m.set_return(1u);
        return true;
    }
    case EM_CANUNDO: commit(); m.set_return(st.can_undo ? 1u : 0u); return true;
    case EM_EMPTYUNDOBUFFER:
        st.can_undo = false;
        st.undo_text.clear();
        commit();
        m.set_return(0u);
        return true;
    case EM_GETMODIFY: commit(); m.set_return(st.modified ? 1u : 0u); return true;
    case EM_SETMODIFY: st.modified = wParam != 0; commit(); m.set_return(0u); return true;

    // ----------------------------------------------------------- selection
    case EM_GETSEL: {
        commit();
        const u32 a = sel_start(), b = sel_end();
        if (wParam != 0) (void)m.write_u32(static_cast<GuestAddr>(wParam), a);
        if (lParam != 0) (void)m.write_u32(static_cast<GuestAddr>(lParam), b);
        m.set_return(a > 0xFFFF || b > 0xFFFF ? 0xFFFFFFFFu : (b << 16) | a);
        return true;
    }
    case EM_SETSEL: {
        // Start -1 deselects (caret stays); end -1 (or any value past the
        // text) means the end; other negative starts are huge unsigned and
        // clamp (edit_messages "setsel(*)"). Single-line puts the caret at
        // the end of the range, multiline at the end argument
        // (edit_messages2 "caret=", "caret_rev=").
        const i32 s = static_cast<i32>(static_cast<u32>(wParam));
        const u32 e = static_cast<u32>(lParam);
        if (s == -1) {
            st.anchor = st.caret;
        } else {
            const u32 a = std::min<u32>(static_cast<u32>(s), len);
            const u32 b = std::min<u32>(e, len);
            if (multi) set_sel(a, b);
            else set_sel(std::min(a, b), std::max(a, b));
        }
        push_selection();
        commit();
        m.set_return(1u);
        return true;
    }
    case EM_GETCARETINDEX: commit(); m.set_return(st.caret); return true;
    case EM_SETCARETINDEX: {
        const u32 c = std::min<u32>(static_cast<u32>(wParam), len);
        set_sel(c, c);
        push_selection();
        commit();
        m.set_return(1u);
        return true;
    }
    case EM_SCROLLCARET:
        commit();
        if (!multi) { m.set_return(0u); return true; }  // edit_messages "scrollcaret=0" single
        (void)m_bridge.edit_scroll_caret(hwnd);
        m.set_return(1u);
        return true;

    // --------------------------------------------------------------- lines
    case EM_GETLINECOUNT: commit(); m.set_return(lines().size()); return true;
    case EM_LINEINDEX: {
        commit();
        if (!multi) { m.set_return(0u); return true; }
        const auto ls = lines();
        const i32 n = static_cast<i32>(static_cast<u32>(wParam));
        if (n == -1) { m.set_return(ls[line_of(ls, st.caret)].start); return true; }
        if (n < 0 || static_cast<usize>(n) >= ls.size()) { m.set_return(0xFFFFFFFFu); return true; }
        m.set_return(ls[static_cast<usize>(n)].start);
        return true;
    }
    case EM_LINELENGTH: {
        commit();
        if (!multi) { m.set_return(len); return true; }
        const auto ls = lines();
        const i32 idx = static_cast<i32>(static_cast<u32>(wParam));
        if (idx == -1) {
            // Unselected characters on the lines the selection touches.
            const Line& first = ls[line_of(ls, sel_start())];
            const Line& last = ls[line_of(ls, sel_end())];
            const u32 before = sel_start() - first.start;
            const u32 after = (last.start + last.length) > sel_end() ? last.start + last.length - sel_end() : 0;
            m.set_return(before + after);
            return true;
        }
        m.set_return(ls[line_of(ls, std::min<u32>(static_cast<u32>(idx), len))].length);
        return true;
    }
    case EM_LINEFROMCHAR: {
        commit();
        if (!multi) { m.set_return(0u); return true; }
        const auto ls = lines();
        const i32 idx = static_cast<i32>(static_cast<u32>(wParam));
        const u32 at = idx == -1 ? sel_start() : std::min<u32>(static_cast<u32>(idx), len);
        m.set_return(line_of(ls, at));
        return true;
    }
    case EM_GETLINE:
    case EM_GETFILELINE: {
        // The buffer's first WORD is its size in TCHARs; no terminator is
        // written; a missing line copies nothing (edit_messages "getline").
        commit();
        const GuestAddr buf = static_cast<GuestAddr>(lParam);
        if (buf == 0) { m.set_return(0u); return true; }
        const u32 capacity = m.read_u16(buf).value_or(0);
        const u32 n = static_cast<u32>(wParam);
        std::u16string text;
        if (!multi) {
            if (msg == EM_GETFILELINE) { m.set_return(0u); return true; }
            text = st.text;
        } else {
            const auto ls = msg == EM_GETLINE ? lines() : cx.hard_lines();
            if (n >= ls.size()) { m.set_return(0u); return true; }
            text = st.text.substr(ls[n].start, ls[n].length);
        }
        if (text.empty()) { m.set_return(0u); return true; }
        m.set_return(copy_no_nul(buf, text, capacity));
        return true;
    }
    case EM_GETFILELINECOUNT: commit(); m.set_return(multi ? cx.hard_lines().size() : 1u); return true;
    case EM_FILELINEINDEX: {
        commit();
        if (!multi) { m.set_return(0u); return true; }
        const auto ls = cx.hard_lines();
        const u32 n = static_cast<u32>(wParam);
        m.set_return(n < ls.size() ? ls[n].start : 0xFFFFFFFFu);
        return true;
    }
    case EM_FILELINELENGTH: {
        commit();
        if (!multi) { m.set_return(0u); return true; }  // edit_messages2 "len(5)=0" single
        const auto ls = cx.hard_lines();
        m.set_return(ls[line_of(ls, std::min<u32>(static_cast<u32>(wParam), len))].length);
        return true;
    }
    case EM_FILELINEFROMCHAR: {
        commit();
        if (!multi) { m.set_return(0u); return true; }
        const auto ls = cx.hard_lines();
        const i32 idx = static_cast<i32>(static_cast<u32>(wParam));
        m.set_return(line_of(ls, idx == -1 ? sel_start() : std::min<u32>(static_cast<u32>(idx), len)));
        return true;
    }
    case EM_FMTLINES:
        st.fmtlines = wParam != 0;
        commit();
        m.set_return(multi ? (wParam != 0 ? 1u : 0u) : 0u);
        return true;

    // ---------------------------------------------------------- scrolling
    case EM_GETFIRSTVISIBLELINE:
        commit();
        m.set_return(multi ? static_cast<u32>(std::max(0, m_bridge.edit_first_visible_line(hwnd))) : 0u);
        return true;
    case EM_GETTHUMB:
        commit();
        m.set_return(multi ? static_cast<u32>(std::max(0, m_bridge.edit_first_visible_line(hwnd))) : 0u);
        return true;
    case EM_LINESCROLL:
        commit();
        if (!multi) { m.set_return(0u); return true; }
        (void)m_bridge.edit_scroll_lines(hwnd, static_cast<i32>(lParam), static_cast<i32>(wParam));
        m.set_return(1u);
        return true;
    case EM_SCROLL: {
        commit();
        if (!multi) { m.set_return(0u); return true; }
        const u32 action = static_cast<u32>(wParam);
        i32 dir = 0;
        bool page = false;
        if (action == SB_LINEUP) dir = -1;
        else if (action == SB_LINEDOWN) dir = 1;
        else if (action == SB_PAGEUP) { dir = -1; page = true; }
        else if (action == SB_PAGEDOWN) { dir = 1; page = true; }
        else { m.set_return(0u); return true; }
        const i32 moved = m_bridge.edit_scroll_lines(hwnd, dir, 0, page);
        // Nothing to scroll returns 0 (edit_messages "scroll_linedown=0").
        m.set_return(moved == 0 ? 0u : (1u << 16) | (static_cast<u32>(moved) & 0xFFFFu));
        return true;
    }

    // ---------------------------------------------------------- geometry
    case EM_GETRECT: {
        commit();
        if (lParam != 0) {
            windowing::Rect r{};
            if (!m_bridge.edit_get_rect(hwnd, r) && win != nullptr) {
                r = {0, 0, win->client_rect.right - win->client_rect.left,
                     win->client_rect.bottom - win->client_rect.top};
            }
            (void)m.write_u32(static_cast<GuestAddr>(lParam) + 0, static_cast<u32>(r.left));
            (void)m.write_u32(static_cast<GuestAddr>(lParam) + 4, static_cast<u32>(r.top));
            (void)m.write_u32(static_cast<GuestAddr>(lParam) + 8, static_cast<u32>(r.right));
            (void)m.write_u32(static_cast<GuestAddr>(lParam) + 12, static_cast<u32>(r.bottom));
        }
        m.set_return(0u);
        return true;
    }
    case EM_SETRECT:
    case EM_SETRECTNP: {
        commit();
        if (multi && win != nullptr) {
            // The formatting rectangle becomes left / right insets; NULL
            // restores the margins.
            if (lParam == 0) {
                m_bridge.edit_set_margins(hwnd, st.left_margin, st.right_margin);
            } else {
                const i32 left = static_cast<i32>(m.read_u32(static_cast<GuestAddr>(lParam)).value_or(0));
                const i32 right = static_cast<i32>(m.read_u32(static_cast<GuestAddr>(lParam) + 8).value_or(0));
                const i32 width = win->client_rect.right - win->client_rect.left;
                m_bridge.edit_set_margins(hwnd, left, std::max(0, width - right));
            }
        }
        m.set_return(0u);
        return true;
    }
    case EM_SETMARGINS: {
        const u32 flags = static_cast<u32>(wParam);
        if (flags == EC_USEFONTINFO) {
            unimplemented_once("EM_SETMARGINS EC_USEFONTINFO: font-derived margins (margins unchanged)");
        } else {
            if (flags & EC_LEFTMARGIN) st.left_margin = static_cast<i16>(lParam & 0xFFFF);
            if (flags & EC_RIGHTMARGIN) st.right_margin = static_cast<i16>((lParam >> 16) & 0xFFFF);
            m_bridge.edit_set_margins(hwnd, st.left_margin, st.right_margin);
        }
        commit();
        m.set_return(0u);
        return true;
    }
    case EM_GETMARGINS:
        commit();
        m.set_return((static_cast<u32>(st.right_margin & 0xFFFF) << 16) |
                     static_cast<u32>(st.left_margin & 0xFFFF));
        return true;
    case EM_POSFROMCHAR: {
        // At or past the end: -1 (edit_messages2 "posfromchar_len_isneg1").
        commit();
        const u32 idx = static_cast<u32>(wParam);
        i32 x = 0, y = 0;
        if (idx >= len || !m_bridge.edit_pos_from_char(hwnd, cx.display().to_display[idx], x, y)) {
            m.set_return(0xFFFFFFFFu);
            return true;
        }
        m.set_return((static_cast<u32>(y & 0xFFFF) << 16) | static_cast<u32>(x & 0xFFFF));
        return true;
    }
    case EM_CHARFROMPOS: {
        commit();
        const i32 x = static_cast<i16>(lParam & 0xFFFF);
        const i32 y = static_cast<i16>((lParam >> 16) & 0xFFFF);
        u32 disp = 0;
        if (!m_bridge.edit_char_from_pos(hwnd, x, y, disp)) { m.set_return(0xFFFFFFFFu); return true; }
        const Display d = cx.display();
        const u32 index = d.to_model[std::min<usize>(disp, d.to_model.size() - 1)];
        if (!multi) { m.set_return(index & 0xFFFF); return true; }
        const auto ls = lines();
        m.set_return((static_cast<u32>(line_of(ls, index)) << 16) | (index & 0xFFFF));
        return true;
    }
    case EM_SETTABSTOPS: {
        // Multiline only (edit_messages "settabstops"). Dialog template
        // units: base unit X is 8 (GetDialogBaseUnits), a quarter of it per
        // unit; no stops -> every 32 units.
        commit();
        if (!multi) { m.set_return(0u); return true; }
        const u32 count = static_cast<u32>(wParam);
        std::vector<f64> points;
        if (count == 0 || lParam == 0) {
            points.push_back(32.0 * 2.0);
        } else {
            for (u32 i = 0; i < count; ++i) {
                const i32 dlu = static_cast<i32>(m.read_u32(static_cast<GuestAddr>(lParam) + i * 4).value_or(0));
                points.push_back(dlu * 2.0);
            }
        }
        m_bridge.edit_set_tab_stops(hwnd, points);
        m.set_return(1u);
        return true;
    }

    // ------------------------------------------------------ style & state
    case EM_SETPASSWORDCHAR: {
        // Sets or clears ES_PASSWORD, multiline too (edit_messages
        // "pwchar=42 style_pw=1"). Single-line rebinds its view.
        st.password_char = static_cast<u16>(wParam);
        if (win != nullptr) {
            const bool had = (win->style & ES_PASSWORD) != 0;
            if (st.password_char != 0) win->style |= ES_PASSWORD;
            else win->style &= ~ES_PASSWORD;
            const bool has = (win->style & ES_PASSWORD) != 0;
            if (!multi && had != has) {
                (void)m_bridge.rebind_child_control(hwnd, *win, 0x0081, L"Edit", to_w(st.text));
            }
        }
        commit();
        m.set_return(0u);
        return true;
    }
    case EM_GETPASSWORDCHAR: commit(); m.set_return(st.password_char); return true;
    case EM_LIMITTEXT: {
        // 0 is "no limit": 0x7FFFFFFE single-line, 0xFFFFFFFF multiline;
        // single-line clamps to 0x7FFFFFFE (edit_messages "limit0",
        // "limitmax"). Existing text is not truncated.
        u32 limit = static_cast<u32>(wParam);
        if (limit == 0) limit = multi ? 0xFFFFFFFFu : 0x7FFFFFFEu;
        else if (!multi && limit > 0x7FFFFFFEu) limit = 0x7FFFFFFEu;
        st.limit = limit;
        commit();
        m.set_return(0u);
        return true;
    }
    case EM_GETLIMITTEXT: commit(); m.set_return(st.limit); return true;
    case EM_SETWORDBREAKPROC:
        st.wordbreak_proc = lParam;
        if (lParam != 0) unimplemented_once("EM_SETWORDBREAKPROC: wrapping ignores the guest word-break procedure");
        commit();
        m.set_return(0u);
        return true;
    case EM_GETWORDBREAKPROC: commit(); m.set_return(st.wordbreak_proc); return true;
    case EM_SETIMESTATUS:
        // No Windows IME: status reads back as 0; SET returns 1
        // (edit_messages2 "setimestatus=1 getimestatus=0").
        commit();
        m.set_return(1u);
        return true;
    case EM_GETIMESTATUS: commit(); m.set_return(0u); return true;
    case EM_ENABLEFEATURE:
        commit();
        m.set_return(wParam <= 1 ? 1u : 0u);  // edit_messages2 "enablefeature0=1 enablefeature9=0"
        return true;
    case EM_SETHANDLE:
        unimplemented_once("EM_SETHANDLE: guest-owned local text buffers (ignored)");
        commit();
        m.set_return(0u);
        return true;
    case EM_GETHANDLE:
        if (multi) unimplemented_once("EM_GETHANDLE: guest-owned local text buffers (returns NULL)");
        commit();
        m.set_return(0u);
        return true;
    case EM_SETCUEBANNER: {
        commit();
        if (multi) { m.set_return(0u); return true; }  // edit_messages "setcue=0" multi
        st.cue = read_text_arg(static_cast<GuestAddr>(lParam));
        st.has_cue = lParam != 0;
        m_bridge.edit_set_cue_banner(hwnd, to_w(st.cue));
        commit();
        m.set_return(1u);
        return true;
    }
    case EM_GETCUEBANNER: {
        // wParam = buffer, lParam = cch. Single-line without a cue writes
        // an empty string and returns 0; multiline returns 0 untouched
        // (edit_messages2 "cue_none").
        commit();
        const GuestAddr buf = static_cast<GuestAddr>(wParam);
        const u32 cch = static_cast<u32>(lParam);
        if (multi || buf == 0 || cch == 0) { m.set_return(0u); return true; }
        const u32 n = st.has_cue ? std::min<u32>(cch - 1, static_cast<u32>(st.cue.size())) : 0;
        for (u32 i = 0; i < n; ++i) (void)m.write_u16(buf + i * 2, static_cast<u16>(st.cue[i]));
        (void)m.write_u16(buf + n * 2, 0);
        m.set_return(st.has_cue ? 1u : 0u);
        return true;
    }
    case EM_SHOWBALLOONTIP:
        unimplemented_once("EM_SHOWBALLOONTIP: balloon tips");
        commit();
        m.set_return(0u);
        return true;
    case EM_HIDEBALLOONTIP: commit(); m.set_return(0u); return true;
    case EM_SETHILITE: commit(); m.set_return(0u); return true;  // Learn: not implemented
    case EM_GETHILITE:
        commit();
        m.set_return(multi ? 0u : 0xFFFFFFFFu);  // edit_messages2 "hilite="
        return true;
    case EM_NOSETFOCUS:
    case EM_TAKEFOCUS:
        unimplemented_once("EM_NOSETFOCUS / EM_TAKEFOCUS");
        commit();
        m.set_return(0u);
        return true;
    case EM_SETEXTENDEDSTYLE: {
        // Single-line faults inside comctl32 on Windows (edit_messages2);
        // Vitreo returns 0 unchanged. Multiline returns the previous styles.
        commit();
        if (!multi) { m.set_return(0u); return true; }
        const u32 mask = wParam != 0 ? static_cast<u32>(wParam) : 0xFFFFFFFFu;
        const u32 previous = st.ex_style;
        st.ex_style = (st.ex_style & ~mask) | (static_cast<u32>(lParam) & mask);
        if (st.ex_style != previous) push_view();
        commit();
        m.set_return(previous);
        return true;
    }
    case EM_GETEXTENDEDSTYLE: commit(); m.set_return(st.ex_style); return true;
    case EM_SETENDOFLINE: {
        const u32 previous = st.end_of_line;
        if (wParam <= 3) st.end_of_line = static_cast<u32>(wParam);
        commit();
        m.set_return(previous);  // edit_messages2 "seteol=1" (the previous mode)
        return true;
    }
    case EM_GETENDOFLINE: commit(); m.set_return(st.end_of_line); return true;
    case EM_ENABLESEARCHWEB:
    case EM_SEARCHWEB:
        unimplemented_once("EM_ENABLESEARCHWEB / EM_SEARCHWEB: web search");
        commit();
        m.set_return(0u);
        return true;
    case EM_GETZOOM:
    case EM_SETZOOM:
        if (st.ex_style & ES_EX_ZOOMABLE) unimplemented_once("EM_GETZOOM / EM_SETZOOM with ES_EX_ZOOMABLE");
        commit();
        m.set_return(0u);  // not zoomable: FALSE, outputs untouched (edit_messages2 "zoom=0")
        return true;
    default:
        commit();
        return false;
    }
}

}  // namespace vitreo::winapi
