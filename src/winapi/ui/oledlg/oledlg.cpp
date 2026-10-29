// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo — oledlg.dll (OLE common dialogs)
//
// OleUIInsertObject / OLEUIINSERTOBJECT:
// https://learn.microsoft.com/en-us/windows/win32/api/oledlg/nf-oledlg-oleuiinsertobjecta
// https://learn.microsoft.com/en-us/windows/win32/api/oledlg/ns-oledlg-oleuiinsertobjecta
//
// What Learn does not say is measured on Windows 11 (x64 and x86):
// tools/oracle_probe/oledlg_insertobject_probe.c, recorded in
// tests/oracle_data/oledlg_insertobject_probe{32,64}.expected.
//  * Validation order: NULL 101; cbStruct != sizeof 103; hWndOwner set and
//    not a window 104; hInstance set without lpszTemplate 108 (hResource is
//    then ignored); hInstance + lpszTemplate not found 110; hResource with no
//    hInstance 112; cClsidExclude with no list 124; lpszFile NULL 116;
//    cchFile 0 or > MAX_PATH 125. lpszCaption, lpfnHook, the exclude list and
//    the creation pointers are not validated (NULL hWndOwner is accepted).
//  * The hook sees WM_INITDIALOG after the library initialised the dialog,
//    with lParam = lCustData; every other message reaches the hook first and
//    a nonzero return stops the library. OLEUI_MSG_ENDDIALOG goes to the hook
//    with wParam = the button (IDOK / IDCANCEL) before the dialog ends; the
//    Help button posts OLEUI_MSG_HELP to hWndOwner with lParam
//    IDD_INSERTOBJECT; Browse sends OLEUI_MSG_BROWSE (wParam MAX_PATH,
//    lParam the file buffer) to the hook, and a nonzero return replaces the
//    file dialog.
//  * The list holds top-level HKCR ProgIDs with an Insertable key (named by
//    their CLSID's default value) or an OLE 1 protocol\StdFileEditing\server
//    (named by the ProgID's default value), one per CLSID, as
//    "<name>\t{CLSID}", sorted, the first selected. A ProgID-level
//    NotInsertable, a missing or unregistered CLSID, or an unnamed class
//    drops the entry; HKCR\CLSID\{x}\Insertable alone lists nothing.
//    IOF_VERIFYSERVERSEXIST drops entries whose server file is missing.
//  * The mode comes from IOF_SELECTCREATECONTROL, then FROMFILE, then NEW;
//    with none of them no radio button is checked and the list stays empty.
//  * OK: sc = S_OK, hMetaPict = the iconic picture or NULL; Create New writes
//    clsid, Create From File writes lpszFile; dwFlags gets CHECKDISPLAYASICON
//    (and, from a file, CHECKLINK) from the check boxes; the SELECT bits are
//    left as passed. A failed creation stores its HRESULT in sc, NULL in
//    *ppvObj and returns OLEUI_IOERR_SCODEHASERROR (the hook still saw
//    ENDDIALOG with IDOK). Cancel and WM_CLOSE write nothing.

#include "oledlg.hpp"
#include "capsule/registry/registry.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>

namespace vitreo::winapi {

namespace {

using namespace oledlg;

constexpr u32 kWmClose = 0x0010;
constexpr u32 kWmInitDialog = 0x0110;
constexpr u32 kWmCommand = 0x0111;
constexpr u32 kBnClicked = 0;
constexpr u32 kLbnSelChange = 1;
constexpr u32 kLbnDblClk = 2;
constexpr u32 kLbAddString = 0x0180;
constexpr u32 kLbResetContent = 0x0184;
constexpr u32 kLbSetCurSel = 0x0186;
constexpr u32 kLbGetCurSel = 0x0188;
constexpr u32 kLbGetText = 0x0189;
constexpr u32 kLbGetTextLen = 0x018A;
constexpr u32 kLbSetTabStops = 0x0192;
constexpr u32 kBmGetCheck = 0x00F0;
constexpr u32 kBmSetCheck = 0x00F1;
constexpr u32 kSwHide = 0;
constexpr u32 kSwShow = 5;
constexpr u32 kIdOk = 1;
constexpr u32 kIdCancel = 2;
constexpr u32 kRtDialog = 5;
constexpr u32 kLmemZeroInit = 0x0040;
constexpr u32 kScratchSize = 0x4000;
constexpr u32 kTemplateArea = 0x1000;
constexpr u32 kInvalidFileAttributes = 0xFFFFFFFFu;
constexpr u32 kOfnFileMustExist = 0x00001000;
constexpr u32 kOfnPathMustExist = 0x00000800;
constexpr u32 kOfnHideReadOnly = 0x00000004;
constexpr u32 kSOk = 0;

void unimplemented(const std::string& what) {
    static std::mutex mu;
    static std::set<std::string> seen;
    std::lock_guard lock(mu);
    if (seen.insert(what).second) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED oledlg.dll!%s\n", what.c_str());
    }
}

std::string utf8(const std::wstring& ws) {
    std::string out;
    for (wchar_t wc : ws) {
        const u32 c = static_cast<u32>(wc);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

std::u16string utf16(std::string_view s) {
    std::u16string out;
    for (usize i = 0; i < s.size();) {
        const u8 b = static_cast<u8>(s[i]);
        u32 c = b;
        usize n = 1;
        if (b >= 0xF0) { c = b & 0x07; n = 4; }
        else if (b >= 0xE0) { c = b & 0x0F; n = 3; }
        else if (b >= 0xC0) { c = b & 0x1F; n = 2; }
        for (usize k = 1; k < n && i + k < s.size(); ++k) c = (c << 6) | (static_cast<u8>(s[i + k]) & 0x3F);
        i += n;
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 | (c >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 | (c & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(c));
        }
    }
    return out;
}

/// Default value of HKCR\<path>, or empty.
std::wstring reg_default(capsule::Registry* reg, const std::wstring& path) {
    auto key = reg->open_key(capsule::HKEY_CLASSES_ROOT, path, false);
    if (key.is_err()) return {};
    std::wstring out;
    auto v = reg->query_value(key.value(), L"");
    if (v.is_ok()) {
        if (const auto* s = std::get_if<std::wstring>(&v.value().data)) out = *s;
    }
    reg->close_key(key.value());
    while (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

std::wstring wide_ascii(std::string_view s) { return std::wstring(s.begin(), s.end()); }

bool reg_has(capsule::Registry* reg, const std::wstring& path) {
    auto key = reg->open_key(capsule::HKEY_CLASSES_ROOT, path, false);
    if (key.is_err()) return false;
    reg->close_key(key.value());
    return true;
}

bool excluded(const std::vector<std::array<u8, 16>>& exclude, const std::array<u8, 16>& clsid) {
    return std::find(exclude.begin(), exclude.end(), clsid) != exclude.end();
}

bool less_ci(const ListEntry& a, const ListEntry& b) {
    const std::string x = a.name + "\t" + guid_string(a.clsid);
    const std::string y = b.name + "\t" + guid_string(b.clsid);
    return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end(), [](char l, char r) {
        return std::tolower(static_cast<unsigned char>(l)) < std::tolower(static_cast<unsigned char>(r));
    });
}

// --- the default Insert Object template (DLGTEMPLATE, oledlg.h ids) --------

struct TemplateWriter {
    std::vector<u8> b;
    void u16v(u16 v) { b.push_back(static_cast<u8>(v)); b.push_back(static_cast<u8>(v >> 8)); }
    void u32v(u32 v) { u16v(static_cast<u16>(v)); u16v(static_cast<u16>(v >> 16)); }
    void str(std::string_view s) { for (char16_t c : utf16(s)) u16v(c); u16v(0); }
    void align4() { while (b.size() % 4) b.push_back(0); }
};

constexpr u32 kWsChild = 0x40000000, kWsVisible = 0x10000000, kWsTabStop = 0x00010000,
              kWsGroup = 0x00020000, kWsBorder = 0x00800000, kWsVScroll = 0x00200000,
              kWsPopup = 0x80000000, kWsCaption = 0x00C00000, kWsSysMenu = 0x00080000;
constexpr u32 kDsModalFrame = 0x80, kDsSetFont = 0x40, kDs3dLook = 0x04;
constexpr u32 kBsPushButton = 0, kBsDefPushButton = 1, kBsAutoCheckBox = 3, kBsRadioButton = 4;
constexpr u32 kLbsNotify = 0x1, kLbsSort = 0x2, kLbsUseTabStops = 0x80;
constexpr u32 kEsAutoHScroll = 0x80;
constexpr u16 kButton = 0x80, kEdit = 0x81, kStatic = 0x82, kListBox = 0x83;

struct Item {
    u16 cls;
    u32 style;
    i16 x, y, cx, cy;
    u32 id;
    const char* text;
};

/// Vitreo's own layout of the documented Insert Object controls. Controls a
/// mode does not use start hidden; set_mode shows them.
std::vector<u8> default_template() {
    const u32 child = kWsChild | kWsVisible;
    const Item items[] = {
        {kButton, child | kWsTabStop | kWsGroup | kBsRadioButton, 6, 18, 74, 10, kIdcCreateNew, "Create &New"},
        {kButton, child | kWsTabStop | kBsRadioButton, 6, 34, 74, 10, kIdcCreateFromFile, "Create from &File"},
        {kButton, kWsChild | kWsTabStop | kBsRadioButton, 6, 50, 74, 10, kIdcInsertControl, "Insert &Control"},
        {kStatic, child, 86, 6, 150, 8, kIdcObjectTypeText, "Object &Type:"},
        {kListBox, child | kWsTabStop | kWsBorder | kWsVScroll | kLbsNotify | kLbsSort | kLbsUseTabStops,
         86, 16, 150, 84, kIdcObjectTypeList, nullptr},
        {kListBox, kWsChild | kWsTabStop | kWsBorder | kWsVScroll | kLbsNotify | kLbsSort | kLbsUseTabStops,
         86, 16, 150, 84, kIdcControlTypeList, nullptr},
        {kStatic, kWsChild, 86, 6, 150, 8, kIdcFileText, "Fi&le:"},
        {kStatic, kWsChild, 86, 16, 150, 8, kIdcFileType, ""},
        {kEdit, kWsChild | kWsTabStop | kWsBorder | kEsAutoHScroll, 86, 28, 150, 12, kIdcFileDisplay, ""},
        {kButton, kWsChild | kWsTabStop | kBsPushButton, 86, 44, 50, 14, kIdcFile, "&Browse..."},
        {kButton, kWsChild | kWsTabStop | kBsAutoCheckBox, 142, 46, 60, 10, kIdcLinkFile, "&Link"},
        {kButton, child | kWsTabStop | kWsGroup | kBsDefPushButton, 244, 6, 56, 14, kIdOk, "OK"},
        {kButton, child | kWsTabStop | kBsPushButton, 244, 24, 56, 14, kIdCancel, "Cancel"},
        {kButton, kWsChild | kWsTabStop | kBsPushButton, 244, 42, 56, 14, kIdcHelp, "&Help"},
        {kButton, kWsChild | kWsTabStop | kBsPushButton, 244, 60, 56, 14, kIdcAddControl, "&Add Control..."},
        {kButton, child | kWsTabStop | kBsAutoCheckBox, 244, 80, 60, 10, kIdcDisplayAsIcon, "&Display As Icon"},
        {kStatic, kWsChild, 244, 94, 56, 32, kIdcIconDisplay, ""},
        {kButton, kWsChild | kWsTabStop | kBsPushButton, 244, 128, 56, 14, kIdcChangeIcon, "Change &Icon..."},
        {kStatic, child, 6, 108, 24, 24, kIdcResultImage, ""},
        {kStatic, child, 34, 108, 202, 34, kIdcResultText, ""},
    };
    TemplateWriter w;
    w.u32v(kWsPopup | kWsCaption | kWsSysMenu | kDsModalFrame | kDsSetFont | kDs3dLook);
    w.u32v(0);
    w.u16v(static_cast<u16>(std::size(items)));
    w.u16v(0); w.u16v(0); w.u16v(306); w.u16v(148);
    w.u16v(0);  // no menu
    w.u16v(0);  // default class
    w.str("Insert Object");
    w.u16v(8);
    w.str("MS Shell Dlg");
    for (const Item& it : items) {
        w.align4();
        w.u32v(it.style);
        w.u32v(0);
        w.u16v(static_cast<u16>(it.x)); w.u16v(static_cast<u16>(it.y));
        w.u16v(static_cast<u16>(it.cx)); w.u16v(static_cast<u16>(it.cy));
        w.u16v(static_cast<u16>(it.id));
        w.u16v(0xFFFF);
        w.u16v(it.cls);
        if (it.text) w.str(it.text); else w.u16v(0);
        w.u16v(0);  // no creation data
    }
    return w.b;
}

} // namespace

namespace oledlg {

std::string guid_string(const std::array<u8, 16>& g) {
    char buf[40];
    const u32 d1 = g[0] | (u32{g[1]} << 8) | (u32{g[2]} << 16) | (u32{g[3]} << 24);
    const u32 d2 = g[4] | (u32{g[5]} << 8);
    const u32 d3 = g[6] | (u32{g[7]} << 8);
    std::snprintf(buf, sizeof buf, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", d1, d2, d3, g[8], g[9],
                  g[10], g[11], g[12], g[13], g[14], g[15]);
    return buf;
}

std::optional<std::array<u8, 16>> parse_guid(std::string_view t) {
    while (!t.empty() && (t.back() == ' ' || t.back() == '\0')) t.remove_suffix(1);
    if (t.size() != 38 || t[0] != '{' || t[37] != '}' || t[9] != '-' || t[14] != '-' || t[19] != '-' ||
        t[24] != '-') {
        return std::nullopt;
    }
    auto hex = [&](usize pos, usize n, u64& out) {
        out = 0;
        for (usize i = 0; i < n; ++i) {
            const char c = t[pos + i];
            u64 v;
            if (c >= '0' && c <= '9') v = static_cast<u64>(c - '0');
            else if (c >= 'a' && c <= 'f') v = static_cast<u64>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v = static_cast<u64>(c - 'A' + 10);
            else return false;
            out = (out << 4) | v;
        }
        return true;
    };
    std::array<u8, 16> g{};
    u64 d1, d2, d3, d4, d5;
    if (!hex(1, 8, d1) || !hex(10, 4, d2) || !hex(15, 4, d3) || !hex(20, 4, d4) || !hex(25, 12, d5)) {
        return std::nullopt;
    }
    for (usize i = 0; i < 4; ++i) g[i] = static_cast<u8>(d1 >> (8 * i));
    for (usize i = 0; i < 2; ++i) g[4 + i] = static_cast<u8>(d2 >> (8 * i));
    for (usize i = 0; i < 2; ++i) g[6 + i] = static_cast<u8>(d3 >> (8 * i));
    g[8] = static_cast<u8>(d4 >> 8);
    g[9] = static_cast<u8>(d4);
    for (usize i = 0; i < 6; ++i) g[10 + i] = static_cast<u8>(d5 >> (8 * (5 - i)));
    return g;
}

} // namespace oledlg

// --- guest calls -----------------------------------------------------------

u64 OledlgManager::call(Marshaler& m, const char* dll, const char* name, std::initializer_list<u64> args,
                        bool* missing) {
    if (missing) *missing = false;
    if (thunks_ == nullptr || !guest_call_) {
        if (missing) *missing = true;
        return 0;
    }
    auto fn = thunks_->get_thunk_address(dll, name);
    if (fn.is_err()) {
        if (missing) *missing = true;
        return 0;
    }
    std::vector<u64> values(args);
    return guest_call_(fn.value(), values.data(), values.size(), m.cpu_is_64bit());
}

GuestAddr OledlgManager::scratch_string(Marshaler& m, Dialog& d, std::string_view text) {
    const std::u16string w = utf16(text);
    const u32 bytes = static_cast<u32>((w.size() + 1) * 2);
    if (d.scratch == 0 || d.scratch_used + bytes > kScratchSize) d.scratch_used = kTemplateArea;
    if (d.scratch == 0 || d.scratch_used + bytes > kScratchSize) return 0;
    const GuestAddr at = d.scratch + d.scratch_used;
    for (usize i = 0; i < w.size(); ++i) (void)m.write_u16(at + i * 2, w[i]);
    (void)m.write_u16(at + w.size() * 2, 0);
    d.scratch_used += (bytes + 7) & ~7u;
    return at;
}

std::string OledlgManager::dlg_item_text(Marshaler& m, Dialog& d, u32 id) {
    const u32 cch = 1024;
    if (d.scratch == 0) return {};
    if (d.scratch_used + cch * 2 > kScratchSize) d.scratch_used = kTemplateArea;
    const GuestAddr buf = d.scratch + d.scratch_used;
    (void)m.write_u16(buf, 0);
    (void)call(m, "user32.dll", "GetDlgItemTextW", {d.hwnd, id, buf, cch});
    auto s = m.read_wide_string(buf, cch);
    return s.is_ok() ? s.value() : std::string{};
}

u32 OledlgManager::item(Marshaler& m, Dialog& d, u32 id) {
    return static_cast<u32>(call(m, "user32.dll", "GetDlgItem", {d.hwnd, id}));
}

void OledlgManager::show(Marshaler& m, Dialog& d, u32 id, bool visible) {
    if (const u32 h = item(m, d, id)) (void)call(m, "user32.dll", "ShowWindow", {h, visible ? kSwShow : kSwHide});
}

void OledlgManager::enable(Marshaler& m, Dialog& d, u32 id, bool enabled) {
    if (const u32 h = item(m, d, id)) (void)call(m, "user32.dll", "EnableWindow", {h, enabled ? 1u : 0u});
}

bool OledlgManager::checked(Marshaler& m, Dialog& d, u32 id) {
    const u32 h = item(m, d, id);
    return h != 0 && (call(m, "user32.dll", "SendMessageW", {h, kBmGetCheck, 0, 0}) & 0xFFFFFFFFu) == 1;
}

void OledlgManager::set_check(Marshaler& m, Dialog& d, u32 id, bool on) {
    if (const u32 h = item(m, d, id)) (void)call(m, "user32.dll", "SendMessageW", {h, kBmSetCheck, on ? 1u : 0u, 0});
}

u64 OledlgManager::call_hook(Marshaler& m, Dialog& d, u32 msg, u64 wp, u64 lp) {
    if (d.hook == 0 || !guest_call_) return 0;
    const u64 args[4] = {d.hwnd, msg, wp, lp};
    // LPFNOLEUIHOOK returns UINT.
    return guest_call_(d.hook, args, 4, m.cpu_is_64bit()) & 0xFFFFFFFFu;
}

// --- the class lists -------------------------------------------------------

std::vector<ListEntry> OledlgManager::insertable_classes(Marshaler& m,
                                                         const std::vector<std::array<u8, 16>>& exclude,
                                                         bool verify) {
    std::vector<ListEntry> out;
    if (registry_ == nullptr) return out;
    auto names = registry_->enum_keys(capsule::HKEY_CLASSES_ROOT);
    if (names.is_err()) return out;
    std::vector<std::array<u8, 16>> seen;
    for (const std::wstring& progid : names.value()) {
        const bool insertable = reg_has(registry_, progid + L"\\Insertable");
        const bool ole1 = !insertable && reg_has(registry_, progid + L"\\protocol\\StdFileEditing\\server");
        if (!insertable && !ole1) continue;
        if (reg_has(registry_, progid + L"\\NotInsertable")) continue;
        const auto clsid = parse_guid(utf8(reg_default(registry_, progid + L"\\CLSID")));
        if (!clsid) continue;
        const std::wstring clsid_key = L"CLSID\\" + wide_ascii(guid_string(*clsid));
        std::wstring name;
        if (insertable) {
            if (!reg_has(registry_, clsid_key)) continue;
            name = reg_default(registry_, clsid_key);
        } else {
            name = reg_default(registry_, progid);
        }
        if (name.empty()) continue;
        if (excluded(exclude, *clsid) || excluded(seen, *clsid)) continue;
        if (verify) {
            std::wstring server;
            if (insertable) {
                for (const wchar_t* sub : {L"\\LocalServer32", L"\\LocalServer", L"\\InprocServer32", L"\\InprocServer"}) {
                    if (reg_has(registry_, clsid_key + sub)) {
                        server = reg_default(registry_, clsid_key + sub);
                        break;
                    }
                }
            } else {
                server = reg_default(registry_, progid + L"\\protocol\\StdFileEditing\\server");
            }
            if (server.empty() || !server_exists(m, utf8(server))) continue;
        }
        seen.push_back(*clsid);
        out.push_back({utf8(name), *clsid});
    }
    std::sort(out.begin(), out.end(), less_ci);
    return out;
}

std::vector<ListEntry> OledlgManager::control_classes(const std::vector<std::array<u8, 16>>& exclude) {
    std::vector<ListEntry> out;
    if (registry_ == nullptr) return out;
    auto root = registry_->open_key(capsule::HKEY_CLASSES_ROOT, L"CLSID", false);
    if (root.is_err()) return out;
    auto names = registry_->enum_keys(root.value());
    registry_->close_key(root.value());
    if (names.is_err()) return out;
    for (const std::wstring& key : names.value()) {
        const auto clsid = parse_guid(utf8(key));
        if (!clsid || excluded(exclude, *clsid)) continue;
        if (!reg_has(registry_, L"CLSID\\" + key + L"\\Control")) continue;
        const std::wstring name = reg_default(registry_, L"CLSID\\" + key);
        if (name.empty()) continue;
        out.push_back({utf8(name), *clsid});
    }
    std::sort(out.begin(), out.end(), less_ci);
    return out;
}

bool OledlgManager::server_exists(Marshaler& m, const std::string& command) {
    // The server value is a command line: a quoted path, or a path that may
    // be followed by arguments.
    std::vector<std::string> candidates;
    if (!command.empty() && command[0] == '"') {
        const auto end = command.find('"', 1);
        candidates.push_back(command.substr(1, end == std::string::npos ? std::string::npos : end - 1));
    } else {
        candidates.push_back(command);
        for (usize sp = command.find(' '); sp != std::string::npos; sp = command.find(' ', sp + 1)) {
            candidates.push_back(command.substr(0, sp));
        }
    }
    const GuestAddr buf = static_cast<GuestAddr>(call(m, "kernel32.dll", "LocalAlloc", {kLmemZeroInit, 2048}));
    if (buf == 0) return false;
    bool found = false;
    for (const std::string& path : candidates) {
        const std::u16string w = utf16(path);
        if (w.size() >= 1000) continue;
        for (usize i = 0; i < w.size(); ++i) (void)m.write_u16(buf + i * 2, w[i]);
        (void)m.write_u16(buf + w.size() * 2, 0);
        GuestAddr probe = buf;
        if (path.find('%') != std::string::npos) {
            const u32 n = static_cast<u32>(call(m, "kernel32.dll", "ExpandEnvironmentStringsW", {buf, buf + 1024, 500}));
            if (n != 0 && n <= 500) probe = buf + 1024;
        }
        const u32 attrs = static_cast<u32>(call(m, "kernel32.dll", "GetFileAttributesW", {probe}));
        if (attrs != kInvalidFileAttributes && (attrs & 0x10) == 0) {
            found = true;
            break;
        }
    }
    (void)call(m, "kernel32.dll", "LocalFree", {buf});
    return found;
}

// --- dialog state ----------------------------------------------------------

void OledlgManager::fill_list(Marshaler& m, Dialog& d, const std::vector<ListEntry>& entries) {
    const u32 list = item(m, d, kIdcObjectTypeList);
    if (list == 0) return;
    (void)call(m, "user32.dll", "SendMessageW", {list, kLbResetContent, 0, 0});
    for (const ListEntry& e : entries) {
        const GuestAddr s = scratch_string(m, d, e.name + "\t" + guid_string(e.clsid));
        if (s != 0) (void)call(m, "user32.dll", "SendMessageW", {list, kLbAddString, 0, s});
    }
    if (!entries.empty()) (void)call(m, "user32.dll", "SendMessageW", {list, kLbSetCurSel, 0, 0});
}

std::optional<std::array<u8, 16>> OledlgManager::selected_clsid(Marshaler& m, Dialog& d) {
    const u32 list = item(m, d, kIdcObjectTypeList);
    if (list == 0) return std::nullopt;
    const i32 sel = static_cast<i32>(call(m, "user32.dll", "SendMessageW", {list, kLbGetCurSel, 0, 0}));
    if (sel < 0) return std::nullopt;
    const i32 len = static_cast<i32>(call(m, "user32.dll", "SendMessageW", {list, kLbGetTextLen, static_cast<u32>(sel), 0}));
    if (len <= 0 || len > 1000 || d.scratch == 0) return std::nullopt;
    d.scratch_used = kTemplateArea;
    const GuestAddr buf = d.scratch + d.scratch_used;
    (void)call(m, "user32.dll", "SendMessageW", {list, kLbGetText, static_cast<u32>(sel), buf});
    auto text = m.read_wide_string(buf, static_cast<usize>(len) + 1);
    if (text.is_err()) return std::nullopt;
    const auto tab = text.value().rfind('\t');
    if (tab == std::string::npos) return std::nullopt;
    return parse_guid(std::string_view(text.value()).substr(tab + 1));
}

void OledlgManager::update_icon_controls(Marshaler& m, Dialog& d) {
    const bool usable = d.mode == Mode::New || d.mode == Mode::File;
    const bool iconic = usable && !(d.flags & kIofDisableDisplayAsIcon) && checked(m, d, kIdcDisplayAsIcon);
    show(m, d, kIdcIconDisplay, iconic);
    show(m, d, kIdcChangeIcon, iconic && !(d.flags & kIofHideChangeIcon));
    enable(m, d, kIdcChangeIcon, iconic);
}

void OledlgManager::update_result(Marshaler& m, Dialog& d) {
    std::string text;
    switch (d.mode) {
    case Mode::New:
        if (selected_clsid(m, d)) {
            text = checked(m, d, kIdcDisplayAsIcon)
                       ? "Inserts a new object of the selected type into your document, displayed as an icon."
                       : "Inserts a new object of the selected type into your document.";
        }
        break;
    case Mode::File:
        text = checked(m, d, kIdcLinkFile)
                   ? "Inserts a picture of the file contents into your document; the picture is linked to the file."
                   : "Inserts the contents of the file as an object into your document.";
        break;
    case Mode::Control:
        if (selected_clsid(m, d)) text = "Inserts a new control of the selected type into your document.";
        break;
    case Mode::None:
        break;
    }
    const GuestAddr s = scratch_string(m, d, text);
    if (s != 0) (void)call(m, "user32.dll", "SetDlgItemTextW", {d.hwnd, kIdcResultText, s});
}

void OledlgManager::set_mode(Marshaler& m, Dialog& d, Mode mode) {
    d.mode = mode;
    const bool file = mode == Mode::File;
    const bool control = mode == Mode::Control;
    set_check(m, d, kIdcCreateNew, mode == Mode::New);
    set_check(m, d, kIdcCreateFromFile, file);
    set_check(m, d, kIdcInsertControl, control);

    for (u32 id : {kIdcObjectTypeList, kIdcObjectTypeText}) {
        show(m, d, id, !file);
        enable(m, d, id, !file);
    }
    for (u32 id : {kIdcFileDisplay, kIdcFile, kIdcFileText, kIdcFileType}) {
        show(m, d, id, file);
        enable(m, d, id, file);
    }
    const bool link = file && !(d.flags & kIofDisableLink);
    show(m, d, kIdcLinkFile, link);
    enable(m, d, kIdcLinkFile, link);
    show(m, d, kIdcAddControl, control);
    enable(m, d, kIdcAddControl, control);
    show(m, d, kIdcControlTypeList, false);

    std::vector<ListEntry> entries;
    if (mode == Mode::New) {
        entries = insertable_classes(m, d.exclude, (d.flags & kIofVerifyServersExist) != 0);
    } else if (control) {
        entries = control_classes(d.exclude);
    }
    if (!file) fill_list(m, d, entries);

    const bool has_selection = !file && !entries.empty();
    const bool icon_box = !(d.flags & kIofDisableDisplayAsIcon) && !control;
    show(m, d, kIdcDisplayAsIcon, icon_box);
    enable(m, d, kIdcDisplayAsIcon, icon_box && (file || has_selection));
    enable(m, d, kIdOk, file || has_selection);
    update_icon_controls(m, d);
    update_result(m, d);
}

void OledlgManager::on_init(Marshaler& m, Dialog& d) {
    const auto& L = m.cpu_is_64bit() ? kInsertObject64 : kInsertObject32;
    // Caption: a NULL or unreadable lpszCaption keeps the template's.
    const GuestAddr caption = static_cast<GuestAddr>(m.read_ptr(d.io + L.caption).value_or(0));
    if (caption != 0) {
        auto text = d.wide ? m.read_wide_string(caption, 512) : m.read_ansi_string(caption, 512);
        if (text.is_ok()) {
            const GuestAddr s = scratch_string(m, d, text.value());
            if (s != 0) (void)call(m, "user32.dll", "SetWindowTextW", {d.hwnd, s});
        }
    }
    if (const u32 list = item(m, d, kIdcObjectTypeList)) {
        // Keep the "\t{CLSID}" half of each item past the visible width.
        (void)m.write_u32(d.scratch + kTemplateArea, 1000);
        (void)call(m, "user32.dll", "SendMessageW", {list, kLbSetTabStops, 1, d.scratch + kTemplateArea});
    }
    show(m, d, kIdcHelp, (d.flags & kIofShowHelp) != 0);
    enable(m, d, kIdcHelp, (d.flags & kIofShowHelp) != 0);
    show(m, d, kIdcInsertControl, (d.flags & kIofShowInsertControl) != 0);
    if (d.flags & kIofHideChangeIcon) {
        if (const u32 h = item(m, d, kIdcChangeIcon)) (void)call(m, "user32.dll", "DestroyWindow", {h});
    }
    if (d.file != 0) {
        auto text = d.wide ? m.read_wide_string(d.file, d.cch_file) : m.read_ansi_string(d.file, d.cch_file);
        const GuestAddr s = scratch_string(m, d, text.is_ok() ? text.value() : std::string{});
        if (s != 0 && (d.flags & kIofSelectCreateFromFile)) {
            (void)call(m, "user32.dll", "SetDlgItemTextW", {d.hwnd, kIdcFileDisplay, s});
        }
    }
    Mode mode = Mode::None;
    if (d.flags & kIofSelectCreateControl) mode = Mode::Control;
    else if (d.flags & kIofSelectCreateFromFile) mode = Mode::File;
    else if (d.flags & kIofSelectCreateNew) mode = Mode::New;
    if (mode == Mode::File || mode == Mode::New) {
        set_check(m, d, kIdcLinkFile, (d.flags & kIofCheckLink) != 0);
        set_check(m, d, kIdcDisplayAsIcon, (d.flags & kIofCheckDisplayAsIcon) != 0);
    }
    set_mode(m, d, mode);
}

void OledlgManager::end_dialog(Marshaler& m, Dialog& d, u32 result) {
    (void)call_hook(m, d, d.msg_enddialog, result == kOleUiCancel ? kIdCancel : kIdOk, 0);
    d.result = result;
    (void)call(m, "user32.dll", "EndDialog", {d.hwnd, result});
}

void OledlgManager::browse(Marshaler& m, Dialog& d) {
    if (d.scratch == 0) return;
    const bool x64 = m.cpu_is_64bit();
    // OLEUI_MSG_BROWSE carries the dialog's own MAX_PATH buffer.
    const GuestAddr buf = d.scratch + kScratchSize - kMaxPath * 2 - 256;
    const GuestAddr ofn = buf + kMaxPath * 2;
    const std::string current = dlg_item_text(m, d, kIdcFileDisplay);
    const std::u16string w = utf16(current).substr(0, kMaxPath - 1);
    if (d.wide) {
        for (usize i = 0; i < w.size(); ++i) (void)m.write_u16(buf + i * 2, w[i]);
        (void)m.write_u16(buf + w.size() * 2, 0);
    } else {
        (void)m.write_ansi_string(buf, current.substr(0, kMaxPath - 1));
    }
    std::optional<std::string> chosen;
    if (call_hook(m, d, d.msg_browse, kMaxPath, buf) != 0) {
        auto s = d.wide ? m.read_wide_string(buf, kMaxPath) : m.read_ansi_string(buf, kMaxPath);
        if (s.is_ok()) chosen = s.value();
    } else {
        // OPENFILENAME (commdlg.h): x86 76 bytes, x64 152.
        const u32 size = x64 ? 152 : 76;
        for (u32 i = 0; i < size; i += 4) (void)m.write_u32(ofn + i, 0);
        (void)m.write_u32(ofn, size);
        (void)m.write_ptr(ofn + (x64 ? 8 : 4), d.hwnd);
        (void)m.write_ptr(ofn + (x64 ? 48 : 28), buf);
        (void)m.write_u32(ofn + (x64 ? 56 : 32), kMaxPath);
        (void)m.write_u32(ofn + (x64 ? 96 : 52), kOfnFileMustExist | kOfnPathMustExist | kOfnHideReadOnly);
        (void)call_hook(m, d, d.msg_browse_ofn, kIdBrowseInsertFile, ofn);
        bool missing = false;
        const u64 ok = call(m, "comdlg32.dll", d.wide ? "GetOpenFileNameW" : "GetOpenFileNameA", {ofn}, &missing);
        if (missing) {
            unimplemented(std::string(d.wide ? "OleUIInsertObjectW" : "OleUIInsertObjectA") +
                          ": Browse needs comdlg32!" + (d.wide ? "GetOpenFileNameW" : "GetOpenFileNameA"));
        } else if ((ok & 0xFFFFFFFFu) != 0) {
            auto s = d.wide ? m.read_wide_string(buf, kMaxPath) : m.read_ansi_string(buf, kMaxPath);
            if (s.is_ok()) chosen = s.value();
        }
    }
    if (chosen) {
        const GuestAddr s = scratch_string(m, d, *chosen);
        if (s != 0) (void)call(m, "user32.dll", "SetDlgItemTextW", {d.hwnd, kIdcFileDisplay, s});
        update_result(m, d);
    }
}

u32 OledlgManager::finish_ok(Marshaler& m, Dialog& d) {
    const auto& L = m.cpu_is_64bit() ? kInsertObject64 : kInsertObject32;
    const char* api = d.wide ? "OleUIInsertObjectW" : "OleUIInsertObjectA";
    const bool iconic = (d.mode == Mode::New || d.mode == Mode::File) &&
                        !(d.flags & kIofDisableDisplayAsIcon) && checked(m, d, kIdcDisplayAsIcon);
    const bool link = d.mode == Mode::File && checked(m, d, kIdcLinkFile);
    u32 flags = m.read_u32(d.io + L.flags).value_or(d.flags);
    GuestAddr meta = 0;
    u32 sc = kSOk;
    bool created = false;
    const GuestAddr ppv = static_cast<GuestAddr>(m.read_ptr(d.io + L.ppv_obj).value_or(0));
    const GuestAddr iid = d.io + L.iid;
    const u64 render = m.read_u32(d.io + L.ole_render).value_or(0);
    const u64 fmt = m.read_ptr(d.io + L.format_etc).value_or(0);
    const u64 site = m.read_ptr(d.io + L.client_site).value_or(0);
    const u64 stg = m.read_ptr(d.io + L.storage).value_or(0);

    auto create = [&](const char* fn, std::initializer_list<u64> args) {
        bool missing = false;
        const u64 hr = call(m, "ole32.dll", fn, args, &missing);
        created = true;
        if (missing) {
            unimplemented(std::string(api) + ": IOF_CREATE* needs ole32!" + fn);
            sc = 0x80004001u;  // E_NOTIMPL
            if (ppv != 0) (void)m.write_ptr(ppv, 0);
        } else {
            sc = static_cast<u32>(hr);
        }
    };

    if (d.mode == Mode::New || d.mode == Mode::Control) {
        if (auto clsid = selected_clsid(m, d)) {
            for (usize i = 0; i < 16; ++i) (void)m.write_u8(d.io + L.clsid + i, (*clsid)[i]);
            if (d.mode == Mode::New && (d.flags & kIofCreateNewObject)) {
                create("OleCreate", {d.io + L.clsid, iid, render, fmt, site, stg, ppv});
            }
            if (iconic) {
                bool missing = false;
                meta = static_cast<GuestAddr>(call(m, "ole32.dll", "OleGetIconOfClass", {d.io + L.clsid, 0, 1}, &missing));
                if (missing) unimplemented(std::string(api) + ": Display As Icon needs ole32!OleGetIconOfClass");
            }
        }
    } else if (d.mode == Mode::File) {
        const std::string text = dlg_item_text(m, d, kIdcFileDisplay);
        if (d.wide) {
            std::u16string w = utf16(text);
            if (w.size() >= d.cch_file) w.resize(d.cch_file - 1);
            for (usize i = 0; i < w.size(); ++i) (void)m.write_u16(d.file + i * 2, w[i]);
            (void)m.write_u16(d.file + w.size() * 2, 0);
        } else {
            (void)m.write_ansi_string(d.file, text.substr(0, d.cch_file - 1));
        }
        // The ole32 file functions take LPCOLESTR: an ANSI caller's name is
        // passed as a wide copy.
        const GuestAddr file_w = d.wide ? d.file : scratch_string(m, d, text.substr(0, d.cch_file - 1));
        if (link && (d.flags & kIofCreateLinkObject)) {
            create("OleCreateLinkToFile", {file_w, iid, render, fmt, site, stg, ppv});
        } else if (!link && (d.flags & kIofCreateFileObject)) {
            // CLSID_NULL: OleCreateFromFile finds the class from the file.
            const GuestAddr clsid_null = scratch_string(m, d, std::string(7, ' '));
            for (u32 i = 0; i < 16; i += 4) (void)m.write_u32(clsid_null + i, 0);
            create("OleCreateFromFile", {clsid_null, file_w, iid, render, fmt, site, stg, ppv});
        }
        if (iconic) {
            bool missing = false;
            meta = static_cast<GuestAddr>(call(m, "ole32.dll", "OleGetIconOfFile", {file_w, link ? 1u : 0u}, &missing));
            if (missing) unimplemented(std::string(api) + ": Display As Icon needs ole32!OleGetIconOfFile");
        }
        flags = link ? (flags | kIofCheckLink) : (flags & ~kIofCheckLink);
    }
    if (d.mode == Mode::New || d.mode == Mode::File) {
        flags = iconic ? (flags | kIofCheckDisplayAsIcon) : (flags & ~kIofCheckDisplayAsIcon);
    }
    (void)m.write_u32(d.io + L.flags, flags);
    (void)m.write_u32(d.io + L.sc, sc);
    (void)m.write_ptr(d.io + L.hmeta_pict, meta);
    if (created && (sc & 0x80000000u)) {
        if (ppv != 0) (void)m.write_ptr(ppv, 0);
        return kIoErrScodeHasError;
    }
    return kOleUiOk;
}

void OledlgManager::on_command(Marshaler& m, Dialog& d, u32 id, u32 code) {
    switch (id) {
    case kIdOk:
        end_dialog(m, d, finish_ok(m, d));
        return;
    case kIdCancel:
        end_dialog(m, d, kOleUiCancel);
        return;
    case kIdcHelp:
        (void)call(m, "user32.dll", "PostMessageW", {d.owner, d.msg_help, d.hwnd, kIddInsertObject});
        return;
    case kIdcCreateNew:
        if (code == kBnClicked && d.mode != Mode::New) set_mode(m, d, Mode::New);
        return;
    case kIdcCreateFromFile:
        if (code == kBnClicked && d.mode != Mode::File) set_mode(m, d, Mode::File);
        return;
    case kIdcInsertControl:
        if (code == kBnClicked && d.mode != Mode::Control) set_mode(m, d, Mode::Control);
        return;
    case kIdcObjectTypeList:
        if (code == kLbnSelChange) {
            const bool sel = selected_clsid(m, d).has_value();
            enable(m, d, kIdOk, sel);
            enable(m, d, kIdcDisplayAsIcon, sel && d.mode == Mode::New && !(d.flags & kIofDisableDisplayAsIcon));
            update_result(m, d);
        } else if (code == kLbnDblClk) {
            end_dialog(m, d, finish_ok(m, d));
        }
        return;
    case kIdcLinkFile:
    case kIdcDisplayAsIcon:
        update_icon_controls(m, d);
        update_result(m, d);
        return;
    case kIdcFile:
        browse(m, d);
        return;
    case kIdcChangeIcon:
        if (call_hook(m, d, d.msg_changeicon, 0, 0) == 0) {
            unimplemented(std::string(d.wide ? "OleUIInsertObjectW" : "OleUIInsertObjectA") +
                          ": Change Icon (OleUIChangeIcon)");
        }
        return;
    case kIdcAddControl:
        if (call_hook(m, d, d.msg_addcontrol, 0, 0) == 0) {
            unimplemented(std::string(d.wide ? "OleUIInsertObjectW" : "OleUIInsertObjectA") +
                          ": Add Control (control registration)");
        }
        return;
    default:
        return;
    }
}

OledlgManager::Dialog* OledlgManager::find_dialog(u32 hwnd) {
    std::lock_guard lock(mutex_);
    auto it = dialogs_.find(hwnd);
    return it == dialogs_.end() ? nullptr : it->second;
}

void OledlgManager::impl_InsertObjectDlgProc(Marshaler& m) {
    const u32 hwnd = m.arg32(0);
    const u32 msg = m.arg32(1);
    const u64 wp = m.arg(2);
    const u64 lp = m.arg(3);
    if (msg == kWmInitDialog) {
        Dialog* d = nullptr;
        {
            std::lock_guard lock(mutex_);
            auto it = pending_.find(lp);
            if (it != pending_.end()) {
                d = it->second;
                pending_.erase(it);
                d->hwnd = hwnd;
                dialogs_[hwnd] = d;
            }
        }
        if (d == nullptr) {
            m.set_return(0);
            return;
        }
        on_init(m, *d);
        // The hook sees WM_INITDIALOG after the library, with lCustData.
        (void)call_hook(m, *d, kWmInitDialog, wp, d->cust_data);
        m.set_return(1);
        return;
    }
    Dialog* d = find_dialog(hwnd);
    if (d == nullptr) {
        m.set_return(0);
        return;
    }
    if (d->hook != 0) {
        const u64 r = call_hook(m, *d, msg, wp, lp);
        if (r != 0) {
            m.set_return(r);
            return;
        }
    }
    if (msg == kWmCommand) {
        on_command(m, *d, static_cast<u32>(wp & 0xFFFF), static_cast<u32>((wp >> 16) & 0xFFFF));
        m.set_return(1);
        return;
    }
    if (msg == kWmClose) {
        end_dialog(m, *d, kOleUiCancel);
        m.set_return(1);
        return;
    }
    m.set_return(0);
}

// --- OleUIInsertObjectW / A ------------------------------------------------

void OledlgManager::impl_OleUIInsertObject(Marshaler& m, bool wide) {
    const GuestAddr io = m.arg_ptr(0);
    const auto& L = m.cpu_is_64bit() ? kInsertObject64 : kInsertObject32;
    const char* api = wide ? "OleUIInsertObjectW" : "OleUIInsertObjectA";
    if (io == 0) {
        m.set_return(kErrStructureNull);
        return;
    }
    auto cb = m.read_u32(io);
    std::vector<u8> probe(L.size);
    if (cb.is_err() || m.memory().read_bytes(io, probe.data(), probe.size()).is_err()) {
        m.set_return(kErrStructureInvalid);
        return;
    }
    if (cb.value() != L.size) {
        m.set_return(kErrCbStructIncorrect);
        return;
    }
    Dialog d;
    d.wide = wide;
    d.io = io;
    d.flags = m.read_u32(io + L.flags).value_or(0);
    d.owner = static_cast<u32>(m.read_ptr(io + L.hwnd_owner).value_or(0));
    d.hook = static_cast<GuestAddr>(m.read_ptr(io + L.hook).value_or(0));
    d.cust_data = m.read_ptr(io + L.cust_data).value_or(0);
    d.file = static_cast<GuestAddr>(m.read_ptr(io + L.file).value_or(0));
    d.cch_file = m.read_u32(io + L.cch_file).value_or(0);
    const GuestAddr hinstance = static_cast<GuestAddr>(m.read_ptr(io + L.hinstance).value_or(0));
    const GuestAddr template_name = static_cast<GuestAddr>(m.read_ptr(io + L.template_name).value_or(0));
    const GuestAddr hresource = static_cast<GuestAddr>(m.read_ptr(io + L.hresource).value_or(0));
    const u32 c_exclude = m.read_u32(io + L.c_clsid_exclude).value_or(0);
    const GuestAddr exclude = static_cast<GuestAddr>(m.read_ptr(io + L.clsid_exclude).value_or(0));

    bool missing = false;
    if (d.owner != 0 && call(m, "user32.dll", "IsWindow", {d.owner}, &missing) == 0) {
        m.set_return(missing ? kErrDialogFailure : kErrHwndOwnerInvalid);
        if (missing) unimplemented(std::string(api) + ": user32!IsWindow is not registered");
        return;
    }

    // Template: hInstance + lpszTemplate (FindResource / LoadResource);
    // hInstance alone is invalid; an hResource without an hInstance fails
    // to make the dialog (measured, see the file header).
    GuestAddr dialog_template = 0;
    GuestAddr dialog_instance = 0;
    if (hinstance != 0 && template_name != 0) {
        const u64 res = call(m, "kernel32.dll", wide ? "FindResourceW" : "FindResourceA",
                             {hinstance, template_name, kRtDialog});
        if (res == 0) {
            m.set_return(kErrFindTemplateFailure);
            return;
        }
        const u64 mem = call(m, "kernel32.dll", "LoadResource", {hinstance, res});
        dialog_template = mem == 0 ? 0 : static_cast<GuestAddr>(call(m, "kernel32.dll", "LockResource", {mem}));
        if (dialog_template == 0) {
            m.set_return(kErrLoadTemplateFailure);
            return;
        }
        dialog_instance = hinstance;
    } else if (hinstance != 0) {
        m.set_return(kErrLpszTemplateInvalid);
        return;
    } else if (hresource != 0) {
        m.set_return(kErrDialogFailure);
        return;
    }

    if (c_exclude != 0 && exclude == 0) {
        m.set_return(kIoErrLpClsidExcludeInvalid);
        return;
    }
    if (d.file == 0) {
        m.set_return(kIoErrLpszFileInvalid);
        return;
    }
    if (d.cch_file == 0 || d.cch_file > kMaxPath) {
        m.set_return(kIoErrCchFileInvalid);
        return;
    }
    for (u32 i = 0; i < c_exclude && i < 4096; ++i) {
        std::array<u8, 16> g{};
        if (m.memory().read_bytes(exclude + i * 16, g.data(), g.size()).is_err()) break;
        d.exclude.push_back(g);
    }

    d.scratch = static_cast<GuestAddr>(call(m, "kernel32.dll", "LocalAlloc", {kLmemZeroInit, kScratchSize}));
    if (d.scratch == 0) {
        m.set_return(kErrLocalMemAlloc);
        return;
    }
    d.scratch_used = kTemplateArea;
    if (dialog_template == 0) {
        const std::vector<u8> t = default_template();
        (void)m.memory().write_bytes(d.scratch, t.data(), t.size());
        dialog_template = d.scratch;
    }
    auto reg_msg = [&](const char* name) {
        const GuestAddr s = scratch_string(m, d, name);
        return static_cast<u32>(call(m, "user32.dll", "RegisterWindowMessageW", {s}));
    };
    d.msg_enddialog = reg_msg("OLEUI_MSG_ENDDIALOG");
    d.msg_browse = reg_msg("OLEUI_MSG_BROWSE");
    d.msg_browse_ofn = reg_msg("OLEUI_MSG_BROWSE_OFN");
    d.msg_help = reg_msg("OLEUI_MSG_HELP");
    d.msg_changeicon = reg_msg("OLEUI_MSG_CHANGEICON");
    d.msg_addcontrol = reg_msg("OLEUI_MSG_ADDCONTROL");

    GuestAddr proc = 0;
    if (thunks_ != nullptr) {
        auto addr = thunks_->get_thunk_address("oledlg.dll", "VitreoOleUIInsertObjectDlgProc", true);
        if (addr.is_ok()) proc = addr.value();
    }
    if (proc == 0) {
        (void)call(m, "kernel32.dll", "LocalFree", {d.scratch});
        unimplemented(std::string(api) + ": dialog procedure thunk is not registered");
        m.set_return(kErrDialogFailure);
        return;
    }
    u64 cookie;
    {
        std::lock_guard lock(mutex_);
        cookie = next_cookie_++;
        pending_[cookie] = &d;
    }
    const u64 r = call(m, "user32.dll", wide ? "DialogBoxIndirectParamW" : "DialogBoxIndirectParamA",
                       {dialog_instance, dialog_template, d.owner, proc, cookie}, &missing);
    {
        std::lock_guard lock(mutex_);
        pending_.erase(cookie);
        if (d.hwnd != 0) dialogs_.erase(d.hwnd);
    }
    (void)call(m, "kernel32.dll", "LocalFree", {d.scratch});
    const i64 result = m.cpu_is_64bit() ? static_cast<i64>(r) : static_cast<i32>(r);
    if (missing || result == -1 || d.hwnd == 0) {
        if (missing) unimplemented(std::string(api) + ": user32 dialog boxes are not registered");
        m.set_return(kErrDialogFailure);
        return;
    }
    // Our own EndDialog carries the result; a hook that ended the dialog
    // itself decides it.
    m.set_return(d.result != 0 ? d.result : static_cast<u32>(result));
}

// --- exports ---------------------------------------------------------------

DllExports create_oledlg_thunks(OledlgManager& mgr) {
    DllExports exports;
    for (const ExportSpec& spec : kExports) {
        const std::string name = spec.name;
        ApiHandler handler;
        if (name == "OleUIInsertObjectW" || name == "OleUIInsertObjectA") {
            const bool wide = name.back() == 'W';
            handler = [&mgr, wide](Marshaler& m) { mgr.impl_OleUIInsertObject(m, wide); };
        } else if (name.rfind("OleUIAddVerbMenu", 0) == 0) {
            // BOOL: FALSE, *lphMenu = NULL.
            handler = [name](Marshaler& m) {
                const GuestAddr out = m.arg_ptr(8);
                if (out != 0) (void)m.write_ptr(out, 0);
                unimplemented(name + ": verb menus (IOleObject::EnumVerbs)");
                m.set_return(0);
            };
        } else if (name == "OleUICanConvertOrActivateAs" || name.rfind("OleUIUpdateLinks", 0) == 0) {
            handler = [name](Marshaler& m) {
                unimplemented(name + ": not implemented");
                m.set_return(0);  // BOOL FALSE
            };
        } else if (name.rfind("OleUIPromptUser", 0) == 0) {
            handler = [name](Marshaler& m) {
                unimplemented(name + ": message dialogs");
                m.set_return(static_cast<u64>(static_cast<u32>(-1)));  // DialogBox failure
            };
        } else {
            // The other OleUI* dialogs take an OLEUIXXX structure: NULL is
            // OLEUI_ERR_STRUCTURENULL, anything else cannot be shown yet.
            handler = [name](Marshaler& m) {
                if (m.arg_ptr(0) == 0) {
                    m.set_return(kErrStructureNull);
                    return;
                }
                unimplemented(name + ": dialog not implemented");
                m.set_return(kErrDialogFailure);
            };
        }
        exports.entries.push_back(ExportEntry{name, spec.ordinal, std::move(handler), spec.stdcall_bytes});
    }
    ExportEntry proc{"VitreoOleUIInsertObjectDlgProc", 0,
                     [&mgr](Marshaler& m) { mgr.impl_InsertObjectDlgProc(m); }, 16};
    proc.internal = true;
    exports.entries.push_back(std::move(proc));
    return exports;
}

} // namespace vitreo::winapi
