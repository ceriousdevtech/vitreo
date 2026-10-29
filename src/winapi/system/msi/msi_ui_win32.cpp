// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_ui_win32.cpp
/// @brief The internal UI's WindowSystem over the msiexec process's user32:
/// the dialog windows are real Win32 windows (CreateWindowExW of Button,
/// Static, RichEdit20W, ComboBox, SysListView32, ...), so Vitreo's user32
/// presents them as native windows like any guest dialog.
///
/// Every call is a call of the guest-visible export, through UiGuestApi.
/// Structure layouts and message numbers: Windows SDK (WinUser.h,
/// CommCtrl.h, RichEdit.h), measured x86 / x64 in
/// tools/oracle_probe/msi_ui_win32_layout*.expected.

#include "winapi/system/msi/msi_ui.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "core/memory/guest_memory.hpp"

#include <deque>
#include <set>
#include <mutex>
#include <unordered_map>

namespace vitreo::winapi::msi {

namespace {

// WinUser.h / CommCtrl.h / RichEdit.h values (msi_ui_win32_layout*.expected
// for the CommCtrl ones).
constexpr u32 WM_CLOSE = 0x0010, WM_SETFONT = 0x0030, WM_DRAWITEM = 0x002B, WM_NOTIFY = 0x004E,
              WM_COMMAND = 0x0111, WM_CTLCOLORBTN = 0x0135, WM_CTLCOLORSTATIC = 0x0138, WM_QUIT = 0x0012;
constexpr u32 BM_SETCHECK = 0x00F1, BM_SETIMAGE = 0x00F7, STM_SETIMAGE = 0x0172, DM_SETDEFID = 0x0401;
constexpr u32 CB_ADDSTRING = 0x0143, CB_RESETCONTENT = 0x014B, CB_SETCURSEL = 0x014E, CB_GETCURSEL = 0x0147,
              CB_GETLBTEXT = 0x0148;
constexpr u32 LB_ADDSTRING = 0x0180, LB_RESETCONTENT = 0x0184, LB_SETCURSEL = 0x0186, LB_GETCURSEL = 0x0188;
constexpr u32 LVM_DELETEALLITEMS = 0x1009, LVM_INSERTITEMW = 0x104D, LVM_INSERTCOLUMNW = 0x1061, LVM_SETITEMSTATE = 0x102B;
constexpr u32 TVM_INSERTITEMW = 0x1132, TVM_DELETEITEM = 0x1101, TVM_SELECTITEM = 0x110B, TVM_HITTEST = 0x1111;
constexpr u32 PBM_SETRANGE32 = 0x0406, PBM_SETPOS = 0x0402, EM_SETTEXTEX = 0x0461, EM_LIMITTEXT = 0x00C5;
constexpr u32 BN_CLICKED = 0, EN_CHANGE = 0x0300, CBN_SELCHANGE = 1, CBN_EDITCHANGE = 5, LBN_SELCHANGE = 1;
constexpr i32 TVN_SELCHANGEDW = -451, NM_CLICK = -2, LVN_ITEMCHANGED = -101;
constexpr u32 TVIF_TEXT = 0x1, TVIF_PARAM = 0x4, TVIF_STATE = 0x8, TVIS_EXPANDED = 0x20, TVGN_CARET = 9,
              TVHT_ONITEMSTATEICON = 0x40, TVHT_ONITEMICON = 0x2;
constexpr u32 LVIF_TEXT = 0x1, LVIF_PARAM = 0x4, LVIF_STATE = 0x8, LVIS_SELECTED = 0x2;
constexpr u32 IMAGE_BITMAP = 0, IMAGE_ICON = 1;
constexpr u32 SW_HIDE = 0, SW_SHOWNOACTIVATE = 4, SW_SHOW = 5;
constexpr u32 SWP_NOSIZE = 0x1, SWP_NOZORDER = 0x4, SWP_NOACTIVATE = 0x10;
constexpr u32 CS_NOCLOSE = 0x200, DLGWINDOWEXTRA = 30, COLOR_BTNFACE = 15;
constexpr u32 TPM_RETURNCMD = 0x100, TPM_NONOTIFY = 0x80, MF_STRING = 0;
constexpr u64 CW_USEDEFAULT = 0x80000000u;

std::u16string to_u16(const std::string& utf8) {
    const std::vector<u16> w = utf8_to_utf16(utf8);
    std::u16string out(w.begin(), w.end());
    while (!out.empty() && out.back() == u'\0') out.pop_back();
    return out;
}
std::string to_utf8(const std::u16string& s) {
    std::vector<u16> w(s.begin(), s.end());
    return utf16_to_utf8(w.data(), static_cast<int>(w.size()));
}

class Win32Windows;

/// The window system creating a window on this thread: its messages arrive
/// (WM_NCCREATE, WM_CREATE, ...) before CreateWindowExW returns the HWND.
thread_local Win32Windows* t_creating = nullptr;

/// HWND -> the window system that created it (the window procedure is one
/// export for every dialog).
std::mutex g_windows_mutex;
std::unordered_map<u64, Win32Windows*>& owners() {
    static std::unordered_map<u64, Win32Windows*> m;
    return m;
}

/// Scratch for one call: freed on scope exit.
class Scratch {
public:
    Scratch(UiGuestApi& api, u32 bytes) : m_api(api), m_base(api.alloc(bytes)), m_size(bytes) {}
    ~Scratch() { if (m_base) m_api.release(m_base); }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    [[nodiscard]] GuestAddr at(u32 offset = 0) const { return m_base ? m_base + offset : 0; }
    [[nodiscard]] bool ok() const { return m_base != 0; }
    void u32_at(u32 off, u32 v) { (void)m_api.memory().write_u32(m_base + off, v); }
    void ptr_at(u32 off, u64 v) {
        if (m_api.is_64bit()) (void)m_api.memory().write_u64(m_base + off, v);
        else (void)m_api.memory().write_u32(m_base + off, static_cast<u32>(v));
    }
    void bytes_at(u32 off, const void* p, usize n) {
        (void)m_api.memory().write_bytes(m_base + off, static_cast<const Byte*>(p), n);
    }
    void wide_at(u32 off, const std::u16string& s) {
        for (usize i = 0; i < s.size(); ++i) (void)m_api.memory().write_u16(m_base + off + i * 2, static_cast<u16>(s[i]));
        (void)m_api.memory().write_u16(m_base + off + static_cast<u32>(s.size()) * 2, 0);
    }
    [[nodiscard]] u32 read32(u32 off) const { return m_api.memory().read_u32(m_base + off).value_or(0); }

private:
    UiGuestApi& m_api;
    GuestAddr m_base;
    u32 m_size;
};

class Win32Windows final : public WindowSystem {
public:
    explicit Win32Windows(std::shared_ptr<UiGuestApi> api) : m_api(std::move(api)), m_x64(m_api->is_64bit()) {}
    ~Win32Windows() override {
        std::lock_guard lock(g_windows_mutex);
        for (auto it = owners().begin(); it != owners().end();)
            it = it->second == this ? owners().erase(it) : std::next(it);
    }

    // -- metrics ---------------------------------------------------------------------
    i32 dpi() const override {
        const u64 dc = c("user32.dll", "GetDC", {0});
        const i32 v = static_cast<i32>(c("gdi32.dll", "GetDeviceCaps", {dc, 90 /*LOGPIXELSY*/}));
        (void)c("user32.dll", "ReleaseDC", {0, dc});
        return v > 0 ? v : 96;
    }
    // The TextHeight property Windows measures (16 at 96 DPI, msi_ui_api
    // probe), scaled with the DPI.
    i32 system_text_height() const override { return 16 * dpi() / 96; }

    std::pair<i32, i32> text_extent(const FontSpec& font, const std::u16string& text) override {
        Scratch s(*m_api, 8 + static_cast<u32>(text.size() + 1) * 2);
        if (!s.ok()) return {0, 0};
        const u64 dc = c("user32.dll", "GetDC", {0});
        const u64 f = font_handle(font);
        const u64 old = f ? c("gdi32.dll", "SelectObject", {dc, f}) : 0;
        s.wide_at(8, text);
        (void)c("gdi32.dll", "GetTextExtentPoint32W", {dc, s.at(8), text.size(), s.at(0)});
        if (f) (void)c("gdi32.dll", "SelectObject", {dc, old});
        (void)c("user32.dll", "ReleaseDC", {0, dc});
        return {static_cast<i32>(s.read32(0)), static_cast<i32>(s.read32(4))};
    }

    // -- windows ---------------------------------------------------------------------
    u64 create_dialog(const DialogWindowSpec& spec, u64 owner) override {
        if (!register_class(spec.class_name, spec.no_close)) return 0;
        Scratch s(*m_api, 16 + static_cast<u32>(spec.class_name.size() + spec.title.size() + 2) * 2);
        if (!s.ok()) return 0;
        // AdjustWindowRectEx for the client size.
        s.u32_at(0, 0), s.u32_at(4, 0), s.u32_at(8, static_cast<u32>(spec.client_w)), s.u32_at(12, static_cast<u32>(spec.client_h));
        (void)c("user32.dll", "AdjustWindowRectEx", {s.at(0), spec.style, 0, spec.ex_style});
        const i32 w = static_cast<i32>(s.read32(8)) - static_cast<i32>(s.read32(0));
        const i32 h = static_cast<i32>(s.read32(12)) - static_cast<i32>(s.read32(4));
        const u32 cls = 16, title = cls + static_cast<u32>(spec.class_name.size() + 1) * 2;
        s.wide_at(cls, spec.class_name);
        s.wide_at(title, spec.title);
        Win32Windows* const outer = t_creating;
        t_creating = this;
        const u64 hwnd = c("user32.dll", "CreateWindowExW",
                           {spec.ex_style, s.at(cls), s.at(title), spec.style, CW_USEDEFAULT, CW_USEDEFAULT,
                            static_cast<u64>(w), static_cast<u64>(h), owner, 0, module(), 0});
        t_creating = outer;
        if (hwnd != 0) own(hwnd, Info{});
        return hwnd;
    }

    u64 create_control(u64 parent, const WindowSpec& spec) override {
        Scratch s(*m_api, static_cast<u32>(spec.class_name.size() + spec.text.size() + 2) * 2);
        if (!s.ok()) return 0;
        const u32 text = static_cast<u32>(spec.class_name.size() + 1) * 2;
        s.wide_at(0, spec.class_name);
        s.wide_at(text, spec.text);
        const u32 id = spec.id != 0 ? spec.id : m_next_id++;
        const u64 hwnd = c("user32.dll", "CreateWindowExW",
                           {spec.ex_style, s.at(0), s.at(text), spec.style, static_cast<u64>(static_cast<u32>(spec.rect.x)),
                            static_cast<u64>(static_cast<u32>(spec.rect.y)), static_cast<u64>(spec.rect.w),
                            static_cast<u64>(spec.rect.h), parent, id, module(), 0});
        if (hwnd == 0) return 0;
        Info info;
        info.cls = to_utf8(spec.class_name);
        info.type = spec.control_type;
        info.transparent = (spec.ex_style & 0x20) != 0;
        info.parent = parent;
        own(hwnd, info);
        if (info.cls == "RichEdit20W") {
            // EN_CHANGE needs ENM_CHANGE in the event mask (RichEdit.h).
            (void)c("user32.dll", "SendMessageW", {hwnd, 0x0445 /*EM_SETEVENTMASK*/, 0, 0x1 /*ENM_CHANGE*/});
        }
        // A child of a group / container window (radio buttons, mask
        // segments): their notifications go to that window, so it is
        // subclassed to forward them.
        subclass_container(parent);
        return hwnd;
    }

    void set_font(u64 w, const FontSpec& f, std::optional<u32> color) override {
        const u64 h = font_handle(f);
        if (h) (void)c("user32.dll", "SendMessageW", {w, WM_SETFONT, h, 1});
        std::lock_guard lock(g_windows_mutex);
        m_info[w].color = color;
    }
    void set_text(u64 w, const std::u16string& t) override {
        Scratch s(*m_api, static_cast<u32>(t.size() + 1) * 2);
        s.wide_at(0, t);
        // The EN_CHANGE this causes is the program's own change, not input.
        {
            std::lock_guard lock(g_windows_mutex);
            m_setting.insert(w);
        }
        (void)c("user32.dll", "SetWindowTextW", {w, s.at(0)});
        std::lock_guard lock(g_windows_mutex);
        m_setting.erase(w);
    }
    void set_rich_text(u64 w, const std::string& rtf) override {
        // EM_SETTEXTEX with SETTEXTEX{ST_DEFAULT, CP_ACP} takes RTF text.
        Scratch s(*m_api, 8 + static_cast<u32>(rtf.size()) + 1);
        s.u32_at(0, 0);
        s.u32_at(4, 0);
        s.bytes_at(8, rtf.data(), rtf.size());
        (void)c("user32.dll", "SendMessageW", {w, EM_SETTEXTEX, s.at(0), s.at(8)});
    }
    void set_visible(u64 w, bool v) override { (void)c("user32.dll", "ShowWindow", {w, v ? SW_SHOW : SW_HIDE}); }
    void set_enabled(u64 w, bool e) override { (void)c("user32.dll", "EnableWindow", {w, e ? 1u : 0u}); }
    void set_checked(u64 w, bool on) override { (void)c("user32.dll", "SendMessageW", {w, BM_SETCHECK, on ? 1u : 0u, 0}); }
    void set_default_button(u64 dialog, u64 button) override {
        const u64 id = c("user32.dll", "GetDlgCtrlID", {button});
        (void)c("user32.dll", "SendMessageW", {dialog, DM_SETDEFID, id, 0});
    }
    void set_focus(u64 w) override { (void)c("user32.dll", "SetFocus", {w}); }

    void set_image(u64 w, const std::vector<u8>& bytes, bool icon, i32 icon_size) override {
        const u64 image = icon ? icon_from(bytes, icon_size) : bitmap_from(bytes);
        if (image == 0) {
            log_unimplemented("InternalUI", icon ? "Binary-table icon not decodable" : "Binary-table bitmap not decodable");
            return;
        }
        set_image_handle(w, image, icon ? IMAGE_ICON : IMAGE_BITMAP);
    }
    void set_system_icon(u64 w, u32 idi) override {
        const u64 icon = c("user32.dll", "LoadIconW", {0, idi});
        if (icon) set_image_handle(w, icon, IMAGE_ICON);
    }

    void set_items(u64 w, const std::vector<std::pair<std::string, std::string>>& items, const std::string& selected) override {
        std::string cls;
        {
            std::lock_guard lock(g_windows_mutex);
            m_info[w].values.clear();
            for (const auto& it : items) m_info[w].values.push_back(it.second);
            cls = m_info[w].cls;
        }
        i64 sel = -1;
        for (usize i = 0; i < items.size(); ++i)
            if (items[i].second == selected) sel = static_cast<i64>(i);
        if (cls == "ComboBox" || cls == "ListBox") {
            const bool combo = cls == "ComboBox";
            (void)c("user32.dll", "SendMessageW", {w, combo ? CB_RESETCONTENT : LB_RESETCONTENT, 0, 0});
            for (const auto& it : items) {
                const std::u16string t = to_u16(it.first);
                Scratch s(*m_api, static_cast<u32>(t.size() + 1) * 2);
                s.wide_at(0, t);
                (void)c("user32.dll", "SendMessageW", {w, combo ? CB_ADDSTRING : LB_ADDSTRING, 0, s.at(0)});
            }
            (void)c("user32.dll", "SendMessageW", {w, combo ? CB_SETCURSEL : LB_SETCURSEL, static_cast<u64>(sel), 0});
            return;
        }
        if (cls == "SysListView32") {
            (void)c("user32.dll", "SendMessageW", {w, LVM_DELETEALLITEMS, 0, 0});
            if (!m_info_columns[w]) {
                // One report column (LVCOLUMNW: mask 0, cx 8, pszText 12/16).
                Scratch col(*m_api, 64);
                col.u32_at(0, 0x2 /*LVCF_WIDTH*/);
                col.u32_at(8, 2000);
                (void)c("user32.dll", "SendMessageW", {w, LVM_INSERTCOLUMNW, 0, col.at(0)});
                m_info_columns[w] = true;
            }
            // LVITEMW: mask 0, iItem 4, pszText 20/24, lParam 32/40.
            const u32 text_off = m_x64 ? 24 : 20, param_off = m_x64 ? 40 : 32, size = m_x64 ? 88 : 60;
            for (usize i = 0; i < items.size(); ++i) {
                const std::u16string t = to_u16(items[i].first);
                Scratch s(*m_api, size + static_cast<u32>(t.size() + 1) * 2);
                s.u32_at(0, LVIF_TEXT | LVIF_PARAM);
                s.u32_at(4, static_cast<u32>(i));
                s.wide_at(size, t);
                s.ptr_at(text_off, s.at(size));
                s.ptr_at(param_off, i);
                (void)c("user32.dll", "SendMessageW", {w, LVM_INSERTITEMW, 0, s.at(0)});
            }
            if (sel >= 0) {
                Scratch s(*m_api, size);
                s.u32_at(0, LVIF_STATE);
                s.u32_at(12, LVIS_SELECTED);  // state
                s.u32_at(16, LVIS_SELECTED);  // stateMask
                (void)c("user32.dll", "SendMessageW", {w, LVM_SETITEMSTATE, static_cast<u64>(sel), s.at(0)});
            }
        }
    }

    void set_progress(u64 w, u32 pos, u32 range) override {
        (void)c("user32.dll", "SendMessageW", {w, PBM_SETRANGE32, 0, range});
        (void)c("user32.dll", "SendMessageW", {w, PBM_SETPOS, pos, 0});
    }

    void set_tree(u64 w, const std::vector<TreeNode>& nodes, const std::string& selected) override {
        constexpr u64 TVI_ROOT_32 = 0xFFFF0000u, TVI_LAST_32 = 0xFFFF0002u;
        const u64 root = m_x64 ? 0xFFFFFFFFFFFF0000ull : TVI_ROOT_32;
        const u64 last = m_x64 ? 0xFFFFFFFFFFFF0002ull : TVI_LAST_32;
        (void)c("user32.dll", "SendMessageW", {w, TVM_DELETEITEM, 0, root});
        std::map<std::string, u64> handles;
        std::vector<TreeNode> copy = nodes;
        {
            std::lock_guard lock(g_windows_mutex);
            m_trees[w].clear();
        }
        // TVINSERTSTRUCTW: hParent 0, hInsertAfter 4/8, item 8/16; TVITEMW:
        // mask 0, state 8/16, stateMask 12/20, pszText 16/24, lParam 36/48.
        const u32 item = m_x64 ? 16 : 8, size = m_x64 ? 96 : 68;
        const u32 state = item + (m_x64 ? 16 : 8), mask_off = state + 4, text = item + (m_x64 ? 24 : 16),
                  param = item + (m_x64 ? 48 : 36);
        u64 select = 0;
        for (usize i = 0; i < copy.size(); ++i) {
            const TreeNode& n = copy[i];
            const std::u16string t = to_u16(n.text);
            Scratch s(*m_api, size + static_cast<u32>(t.size() + 1) * 2);
            const auto p = handles.find(n.parent);
            s.ptr_at(0, p != handles.end() ? p->second : root);
            s.ptr_at(m_x64 ? 8 : 4, last);
            s.u32_at(item, TVIF_TEXT | TVIF_PARAM | TVIF_STATE);
            s.u32_at(state, n.expanded ? TVIS_EXPANDED : 0);
            s.u32_at(mask_off, TVIS_EXPANDED);
            s.wide_at(size, t);
            s.ptr_at(text, s.at(size));
            s.ptr_at(param, i);
            const u64 h = c("user32.dll", "SendMessageW", {w, TVM_INSERTITEMW, 0, s.at(0)});
            handles[n.key] = h;
            if (n.key == selected) select = h;
        }
        {
            std::lock_guard lock(g_windows_mutex);
            for (const auto& [k, h] : handles) m_trees[w][h] = k;
            m_tree_nodes[w] = std::move(copy);
        }
        if (select) (void)c("user32.dll", "SendMessageW", {w, TVM_SELECTITEM, TVGN_CARET, select});
    }

    void move(u64 w, const PixelRect& r) override {
        (void)c("user32.dll", "MoveWindow", {w, static_cast<u32>(r.x), static_cast<u32>(r.y), static_cast<u32>(r.w), static_cast<u32>(r.h), 1});
    }

    void show_dialog(u64 d, i32 hcenter, i32 vcenter, bool activate) override {
        Scratch s(*m_api, 32);
        (void)c("user32.dll", "GetWindowRect", {d, s.at(0)});
        const i32 w = static_cast<i32>(s.read32(8)) - static_cast<i32>(s.read32(0));
        const i32 h = static_cast<i32>(s.read32(12)) - static_cast<i32>(s.read32(4));
        (void)c("user32.dll", "SystemParametersInfoW", {0x30 /*SPI_GETWORKAREA*/, 0, s.at(16), 0});
        const i32 left = static_cast<i32>(s.read32(16)), top = static_cast<i32>(s.read32(20));
        const i32 ww = static_cast<i32>(s.read32(24)) - left, wh = static_cast<i32>(s.read32(28)) - top;
        // HCentering / VCentering: 0 = left/top edge, 100 = right/bottom.
        const i32 x = left + (ww - w) * hcenter / 100, y = top + (wh - h) * vcenter / 100;
        (void)c("user32.dll", "SetWindowPos", {d, 0, static_cast<u32>(x), static_cast<u32>(y), 0, 0,
                                               SWP_NOSIZE | SWP_NOZORDER | (activate ? 0u : SWP_NOACTIVATE)});
        (void)c("user32.dll", "ShowWindow", {d, activate ? SW_SHOW : SW_SHOWNOACTIVATE});
    }

    void destroy(u64 w) override {
        (void)c("user32.dll", "DestroyWindow", {w});
        std::lock_guard lock(g_windows_mutex);
        owners().erase(w);
        m_info.erase(w);
    }

    UiInput wait_input(u64 dialog, u32 timeout_ms) override {
        for (;;) {
            if (auto in = take(dialog)) return *in;
            // MsgWaitForMultipleObjects(0, NULL, FALSE, timeout, QS_ALLINPUT)
            const u64 r = c("user32.dll", "MsgWaitForMultipleObjects", {0, 0, 0, timeout_ms, 0x04FF});
            if (static_cast<u32>(r) == 0x102 /*WAIT_TIMEOUT*/) return {};
            if (!pump_messages(dialog)) return {UiInput::Kind::Close, 0, {}};
            if (auto in = take(dialog)) return *in;
            if (timeout_ms == 0) return {};
        }
    }

    void pump() override { (void)pump_messages(0); }

    i32 message_box(u64 owner, const std::u16string& text, const std::u16string& caption, u32 type) override {
        Scratch s(*m_api, static_cast<u32>(text.size() + caption.size() + 2) * 2);
        s.wide_at(0, text);
        const u32 cap = static_cast<u32>(text.size() + 1) * 2;
        s.wide_at(cap, caption);
        return static_cast<i32>(c("user32.dll", "MessageBoxW", {owner, s.at(0), s.at(cap), type}));
    }

    // -- the window procedure ----------------------------------------------------------
    u64 window_proc(u64 hwnd, u32 msg, u64 wp, u64 lp, bool& handled) {
        handled = true;
        switch (msg) {
            case WM_COMMAND: {
                const u32 code = static_cast<u32>(wp >> 16) & 0xFFFF, id = static_cast<u32>(wp & 0xFFFF);
                const u64 ctl = lp;
                if (ctl == 0) {
                    // IsDialogMessage's Esc / Return.
                    if (id == 2) queue(hwnd, {UiInput::Kind::Close, 0, {}});
                    else if (id == 1) {
                        const u64 def = c("user32.dll", "SendMessageW", {hwnd, 0x0400 /*DM_GETDEFID*/, 0, 0});
                        if ((def >> 16) == 0x534B) {
                            const u64 b = c("user32.dll", "GetDlgItem", {hwnd, def & 0xFFFF});
                            if (b) queue(hwnd, {UiInput::Kind::Activated, b, {}});
                        }
                    }
                    break;
                }
                command(hwnd, ctl, code);
                break;
            }
            case WM_NOTIFY: notify(hwnd, lp); break;
            case WM_CLOSE: queue(hwnd, {UiInput::Kind::Close, 0, {}}); break;
            case WM_DRAWITEM: return draw_item(lp);
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLORBTN: {
                Info info;
                {
                    std::lock_guard lock(g_windows_mutex);
                    auto it = m_info.find(lp);
                    if (it == m_info.end() || (!it->second.color && !it->second.transparent)) {
                        handled = false;
                        return 0;
                    }
                    info = it->second;
                }
                if (info.color) (void)c("gdi32.dll", "SetTextColor", {wp, *info.color});
                if (info.transparent) {
                    (void)c("gdi32.dll", "SetBkMode", {wp, 1 /*TRANSPARENT*/});
                    return c("gdi32.dll", "GetStockObject", {5 /*NULL_BRUSH*/});
                }
                return c("user32.dll", "GetSysColorBrush", {COLOR_BTNFACE});
            }
            default: handled = false; return 0;
        }
        return 0;
    }

    [[nodiscard]] u64 previous_proc(u64 hwnd) {
        std::lock_guard lock(g_windows_mutex);
        auto it = m_subclassed.find(hwnd);
        return it == m_subclassed.end() ? 0 : it->second;
    }

    u64 c(std::string_view dll, std::string_view fn, const std::vector<u64>& args) const { return m_api->call(dll, fn, args); }

private:
    struct Info {
        std::string cls, type;
        std::optional<u32> color;
        bool transparent{false};
        u64 parent{0};
        std::vector<std::string> values;
    };

    void own(u64 hwnd, Info info) {
        std::lock_guard lock(g_windows_mutex);
        owners()[hwnd] = this;
        m_info[hwnd] = std::move(info);
    }

    u64 module() const { return c("kernel32.dll", "GetModuleHandleW", {0}); }

    bool register_class(const std::u16string& name, bool no_close) {
        const std::string key = to_utf8(name);
        if (m_classes.contains(key)) return true;
        // WNDCLASSEXW: cbSize 0, style 4, lpfnWndProc 8, cbWndExtra 16/20,
        // hCursor 28/40, hbrBackground 32/48, lpszClassName 40/64.
        const u32 size = m_x64 ? 80 : 48;
        Scratch s(*m_api, size + static_cast<u32>(name.size() + 1) * 2);
        if (!s.ok()) return false;
        s.u32_at(0, size);
        s.u32_at(4, no_close ? CS_NOCLOSE : 0);
        s.ptr_at(8, m_api->window_proc());
        s.u32_at(m_x64 ? 20 : 16, DLGWINDOWEXTRA);
        s.ptr_at(m_x64 ? 24 : 20, module());
        s.ptr_at(m_x64 ? 40 : 28, c("user32.dll", "LoadCursorW", {0, 32512 /*IDC_ARROW*/}));
        s.ptr_at(m_x64 ? 48 : 32, COLOR_BTNFACE + 1);
        s.wide_at(size, name);
        s.ptr_at(m_x64 ? 64 : 40, s.at(size));
        if (c("user32.dll", "RegisterClassExW", {s.at(0)}) == 0) return false;
        m_classes.insert(key);
        return true;
    }

    void subclass_container(u64 parent) {
        {
            std::lock_guard lock(g_windows_mutex);
            auto it = m_info.find(parent);
            if (it == m_info.end() || it->second.cls.empty() || m_subclassed.contains(parent)) return;  // a dialog, or done
        }
        const i32 GWLP_WNDPROC = -4;
        const u64 old = c("user32.dll", m_x64 ? "SetWindowLongPtrW" : "SetWindowLongW",
                          {parent, static_cast<u64>(static_cast<u32>(GWLP_WNDPROC)), m_api->window_proc()});
        std::lock_guard lock(g_windows_mutex);
        m_subclassed[parent] = old;
    }

    /// The dialog (top-level) window above @p hwnd.
    u64 dialog_of(u64 hwnd) {
        std::lock_guard lock(g_windows_mutex);
        u64 h = hwnd;
        for (int i = 0; i < 8; ++i) {
            auto it = m_info.find(h);
            if (it == m_info.end() || it->second.cls.empty()) return h;
            h = it->second.parent;
        }
        return h;
    }

    void queue(u64 from, UiInput in) {
        std::lock_guard lock(m_queue_mutex);
        m_queue.emplace_back(dialog_of(from), std::move(in));
    }
    std::optional<UiInput> take(u64 dialog) {
        std::lock_guard lock(m_queue_mutex);
        for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
            if (it->first == dialog || dialog == 0) {
                UiInput in = it->second;
                m_queue.erase(it);
                return in;
            }
        }
        return std::nullopt;
    }

    std::u16string window_text(u64 w) {
        const u32 n = static_cast<u32>(c("user32.dll", "GetWindowTextLengthW", {w}));
        Scratch s(*m_api, (n + 1) * 2);
        (void)c("user32.dll", "GetWindowTextW", {w, s.at(0), n + 1});
        std::u16string out;
        for (u32 i = 0; i < n; ++i) out.push_back(static_cast<char16_t>(m_api->memory().read_u16(s.at(i * 2)).value_or(0)));
        return out;
    }

    std::string value_at(u64 w, i64 index) {
        std::lock_guard lock(g_windows_mutex);
        auto it = m_info.find(w);
        if (it == m_info.end() || index < 0 || static_cast<usize>(index) >= it->second.values.size()) return {};
        return it->second.values[static_cast<usize>(index)];
    }

    void command(u64 from, u64 ctl, u32 code) {
        Info info;
        {
            std::lock_guard lock(g_windows_mutex);
            auto it = m_info.find(ctl);
            if (it == m_info.end()) return;
            info = it->second;
        }
        if (info.cls == "Button" && code == BN_CLICKED) {
            // A radio button inside a group reports a value change; push
            // buttons and check boxes an activation.
            const bool radio = info.type.empty();
            queue(from, {radio ? UiInput::Kind::Changed : UiInput::Kind::Activated, ctl, {}});
        } else if ((info.cls == "Edit" || info.cls == "RichEdit20W") && code == EN_CHANGE) {
            {
                std::lock_guard lock(g_windows_mutex);
                if (m_setting.contains(ctl)) return;
            }
            queue(from, {UiInput::Kind::Changed, ctl, to_utf8(window_text(ctl))});
        } else if (info.cls == "ComboBox" && code == CBN_SELCHANGE) {
            const i64 sel = static_cast<i32>(c("user32.dll", "SendMessageW", {ctl, CB_GETCURSEL, 0, 0}));
            queue(from, {UiInput::Kind::Changed, ctl, value_at(ctl, sel)});
        } else if (info.cls == "ComboBox" && code == CBN_EDITCHANGE) {
            queue(from, {UiInput::Kind::Changed, ctl, to_utf8(window_text(ctl))});
        } else if (info.cls == "ListBox" && code == LBN_SELCHANGE) {
            const i64 sel = static_cast<i32>(c("user32.dll", "SendMessageW", {ctl, LB_GETCURSEL, 0, 0}));
            queue(from, {UiInput::Kind::Changed, ctl, value_at(ctl, sel)});
        }
    }

    void notify(u64 from, u64 nmhdr) {
        GuestMemory& mem = m_api->memory();
        const u64 ctl = m_x64 ? mem.read_u64(nmhdr).value_or(0) : mem.read_u32(nmhdr).value_or(0);
        const i32 code = static_cast<i32>(mem.read_u32(nmhdr + (m_x64 ? 16 : 8)).value_or(0));
        Info info;
        {
            std::lock_guard lock(g_windows_mutex);
            auto it = m_info.find(ctl);
            if (it == m_info.end()) return;
            info = it->second;
        }
        if (info.cls == "SysTreeView32" && code == TVN_SELCHANGEDW) {
            // NMTREEVIEWW.itemNew (56/88) .hItem (+4/+8).
            const GuestAddr at = nmhdr + (m_x64 ? 88 + 8 : 56 + 4);
            const u64 item = m_x64 ? mem.read_u64(at).value_or(0) : mem.read_u32(at).value_or(0);
            std::string key;
            {
                std::lock_guard lock(g_windows_mutex);
                key = m_trees[ctl][item];
            }
            queue(from, {UiInput::Kind::Selected, ctl, key});
        } else if (info.cls == "SysTreeView32" && code == NM_CLICK) {
            tree_menu(from, ctl);
        } else if (info.cls == "SysListView32" && code == LVN_ITEMCHANGED) {
            // NMLISTVIEW: iItem 12/24, uNewState 20/32, uChanged 28/40.
            const i32 index = static_cast<i32>(mem.read_u32(nmhdr + (m_x64 ? 24 : 12)).value_or(0));
            const u32 state = mem.read_u32(nmhdr + (m_x64 ? 32 : 20)).value_or(0);
            const u32 changed = mem.read_u32(nmhdr + (m_x64 ? 40 : 28)).value_or(0);
            if ((changed & LVIF_STATE) && (state & LVIS_SELECTED)) {
                const bool directory = info.type == "DirectoryList";
                queue(from, {directory ? UiInput::Kind::Selected : UiInput::Kind::Changed, ctl, value_at(ctl, index)});
            }
        }
    }

    /// A click on a tree item's icon opens its install-state menu.
    void tree_menu(u64 from, u64 tree) {
        Scratch s(*m_api, 32);
        (void)c("user32.dll", "GetCursorPos", {s.at(0)});
        const u32 sx = s.read32(0), sy = s.read32(4);
        s.u32_at(8, sx), s.u32_at(12, sy);
        (void)c("user32.dll", "ScreenToClient", {tree, s.at(8)});
        // TVHITTESTINFO: pt 0, flags 8, hItem 12/16.
        Scratch hit(*m_api, 24);
        hit.u32_at(0, s.read32(8));
        hit.u32_at(4, s.read32(12));
        const u64 item = c("user32.dll", "SendMessageW", {tree, TVM_HITTEST, 0, hit.at(0)});
        const u32 flags = hit.read32(8);
        if (item == 0 || !(flags & (TVHT_ONITEMICON | TVHT_ONITEMSTATEICON))) return;
        std::string key;
        std::vector<std::pair<std::string, std::string>> menu;
        {
            std::lock_guard lock(g_windows_mutex);
            key = m_trees[tree][item];
            for (const TreeNode& n : m_tree_nodes[tree])
                if (n.key == key) menu = n.menu;
        }
        if (menu.empty()) return;
        const u64 popup = c("user32.dll", "CreatePopupMenu", {});
        for (usize i = 0; i < menu.size(); ++i) {
            const std::u16string t = to_u16(menu[i].second.empty() ? menu[i].first : menu[i].second);
            Scratch ts(*m_api, static_cast<u32>(t.size() + 1) * 2);
            ts.wide_at(0, t);
            (void)c("user32.dll", "AppendMenuW", {popup, MF_STRING, i + 1, ts.at(0)});
        }
        const u64 pick = c("user32.dll", "TrackPopupMenu", {popup, TPM_RETURNCMD | TPM_NONOTIFY, sx, sy, 0, from, 0});
        (void)c("user32.dll", "DestroyMenu", {popup});
        if (pick >= 1 && pick <= menu.size()) queue(from, {UiInput::Kind::Changed, tree, key + "\t" + menu[pick - 1].first});
    }

    /// Owner-drawn windows: the invisible box of a borderless
    /// RadioButtonGroup draws nothing; the directory / volume combo boxes
    /// draw their item text.
    u64 draw_item(u64 dis) {
        GuestMemory& mem = m_api->memory();
        const u32 type = mem.read_u32(dis).value_or(0);  // ODT_BUTTON 4, ODT_COMBOBOX 3
        if (type == 4) return 1;
        if (type != 3) return 0;
        const i32 item = static_cast<i32>(mem.read_u32(dis + 8).value_or(0));
        const GuestAddr hwnd_at = dis + (m_x64 ? 24 : 20), dc_at = dis + (m_x64 ? 32 : 24), rc_at = dis + (m_x64 ? 40 : 28);
        const u64 hwnd = m_x64 ? mem.read_u64(hwnd_at).value_or(0) : mem.read_u32(hwnd_at).value_or(0);
        const u64 dc = m_x64 ? mem.read_u64(dc_at).value_or(0) : mem.read_u32(dc_at).value_or(0);
        if (item < 0) return 1;
        Scratch s(*m_api, 1040);
        const i32 n = static_cast<i32>(c("user32.dll", "SendMessageW", {hwnd, CB_GETLBTEXT, static_cast<u64>(item), s.at(16)}));
        if (n <= 0) return 1;
        (void)c("user32.dll", "DrawTextW", {dc, s.at(16), static_cast<u64>(n), rc_at, 0x24 /*DT_SINGLELINE|DT_VCENTER*/});
        return 1;
    }

    bool pump_messages(u64 dialog) {
        Scratch msg(*m_api, 48);
        for (;;) {
            if (c("user32.dll", "PeekMessageW", {msg.at(0), 0, 0, 0, 1 /*PM_REMOVE*/}) == 0) return true;
            if (msg.read32(m_x64 ? 8 : 4) == WM_QUIT) return false;
            if (dialog != 0 && c("user32.dll", "IsDialogMessageW", {dialog, msg.at(0)}) != 0) continue;
            (void)c("user32.dll", "TranslateMessage", {msg.at(0)});
            (void)c("user32.dll", "DispatchMessageW", {msg.at(0)});
        }
    }

    u64 font_handle(const FontSpec& f) {
        const std::string key = to_utf8(f.face) + "|" + std::to_string(f.height) + "|" + std::to_string(f.weight) +
                                (f.italic ? "i" : "") + (f.underline ? "u" : "") + (f.strike ? "s" : "");
        if (auto it = m_fonts.find(key); it != m_fonts.end()) return it->second;
        // LOGFONTW: lfHeight 0, lfWeight 16, lfItalic 20, lfUnderline 21,
        // lfStrikeOut 22, lfCharSet 23, lfFaceName 28 (32 WCHAR).
        Scratch s(*m_api, 92);
        if (!s.ok()) return 0;
        s.u32_at(0, static_cast<u32>(f.height));
        s.u32_at(16, static_cast<u32>(f.weight));
        const u8 flags[4] = {static_cast<u8>(f.italic), static_cast<u8>(f.underline), static_cast<u8>(f.strike), 1 /*DEFAULT_CHARSET*/};
        s.bytes_at(20, flags, 4);
        s.wide_at(28, f.face.substr(0, 31));
        const u64 h = c("gdi32.dll", "CreateFontIndirectW", {s.at(0)});
        if (h) m_fonts[key] = h;
        return h;
    }

    void set_image_handle(u64 w, u64 image, u32 kind) {
        std::string cls;
        {
            std::lock_guard lock(g_windows_mutex);
            cls = m_info[w].cls;
        }
        (void)c("user32.dll", "SendMessageW", {w, cls == "Button" ? BM_SETIMAGE : STM_SETIMAGE, kind, image});
    }

    u64 bitmap_from(const std::vector<u8>& bmp) {
        // A .bmp file: BITMAPFILEHEADER (14 bytes: bfOffBits at 10), then the DIB.
        if (bmp.size() < 54 || bmp[0] != 'B' || bmp[1] != 'M') return 0;
        const u32 off = static_cast<u32>(bmp[10] | (bmp[11] << 8) | (bmp[12] << 16) | (bmp[13] << 24));
        if (off >= bmp.size()) return 0;
        Scratch s(*m_api, static_cast<u32>(bmp.size()));
        if (!s.ok()) return 0;
        s.bytes_at(0, bmp.data(), bmp.size());
        const u64 dc = c("user32.dll", "GetDC", {0});
        // CreateDIBitmap(hdc, &bih, CBM_INIT, bits, &bmi, DIB_RGB_COLORS).
        const u64 h = c("gdi32.dll", "CreateDIBitmap", {dc, s.at(14), 4, s.at(off), s.at(14), 0});
        (void)c("user32.dll", "ReleaseDC", {0, dc});
        return h;
    }

    u64 icon_from(const std::vector<u8>& ico, i32 size) {
        // ICONDIR (6 bytes) + ICONDIRENTRY (16 each: width 0, height 1,
        // bytes 8, offset 12); the requested size's image, else the first.
        if (ico.size() < 22) return 0;
        const u32 count = static_cast<u32>(ico[4] | (ico[5] << 8));
        usize pick = 0;
        for (u32 i = 0; i < count && 6 + i * 16 + 16 <= ico.size(); ++i) {
            const i32 w = ico[6 + i * 16] == 0 ? 256 : ico[6 + i * 16];
            if (size != 0 && w == size) {
                pick = i;
                break;
            }
        }
        const u8* e = ico.data() + 6 + pick * 16;
        const u32 bytes = static_cast<u32>(e[8] | (e[9] << 8) | (e[10] << 16) | (e[11] << 24));
        const u32 off = static_cast<u32>(e[12] | (e[13] << 8) | (e[14] << 16) | (e[15] << 24));
        if (static_cast<u64>(off) + bytes > ico.size()) return 0;
        Scratch s(*m_api, bytes);
        if (!s.ok()) return 0;
        s.bytes_at(0, ico.data() + off, bytes);
        const i32 cx = size != 0 ? size : (e[0] == 0 ? 256 : e[0]);
        return c("user32.dll", "CreateIconFromResourceEx", {s.at(0), bytes, 1, 0x00030000, static_cast<u64>(cx), static_cast<u64>(cx), 0});
    }

    std::shared_ptr<UiGuestApi> m_api;
    bool m_x64;
    u32 m_next_id{1000};
    std::set<std::string> m_classes;
    std::map<std::string, u64> m_fonts;
    std::map<u64, Info> m_info;  // guarded by g_windows_mutex
    std::map<u64, bool> m_info_columns;
    std::map<u64, u64> m_subclassed;
    std::set<u64> m_setting;
    std::map<u64, std::map<u64, std::string>> m_trees;
    std::map<u64, std::vector<TreeNode>> m_tree_nodes;
    std::mutex m_queue_mutex;
    std::deque<std::pair<u64, UiInput>> m_queue;
};

}  // namespace

std::shared_ptr<WindowSystem> make_user32_window_system(std::shared_ptr<UiGuestApi> api) {
    return std::make_shared<Win32Windows>(std::move(api));
}

void ui_window_proc(Marshaler& m) {
    const u64 hwnd = m.arg(0);
    const u32 msg = m.arg32(1);
    const u64 wp = m.arg(2), lp = m.arg(3);
    Win32Windows* owner = nullptr;
    {
        std::lock_guard lock(g_windows_mutex);
        auto it = owners().find(hwnd);
        if (it != owners().end()) owner = it->second;
    }
    if (owner == nullptr) {
        // Messages before CreateWindowExW returned (WM_NCCREATE, WM_CREATE,
        // WM_NCCALCSIZE...): the dialog default procedure.
        if (t_creating != nullptr) {
            m.set_return(t_creating->c("user32.dll", "DefDlgProcW", {hwnd, msg, wp, lp}));
            return;
        }
        log_unimplemented("ui_window_proc", "message for a window no MSI window system owns");
        m.set_return(0);
        return;
    }
    bool handled = false;
    const u64 r = owner->window_proc(hwnd, msg, wp, lp, handled);
    if (handled) {
        m.set_return(r);
        return;
    }
    if (const u64 prev = owner->previous_proc(hwnd); prev != 0) {
        m.set_return(owner->c("user32.dll", "CallWindowProcW", {prev, hwnd, msg, wp, lp}));
        return;
    }
    m.set_return(owner->c("user32.dll", "DefDlgProcW", {hwnd, msg, wp, lp}));
}

}  // namespace vitreo::winapi::msi
