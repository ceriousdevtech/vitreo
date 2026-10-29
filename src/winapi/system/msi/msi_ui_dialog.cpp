// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_ui_dialog.cpp
/// @brief Windows Installer internal UI: the dialog model read from the UI
/// tables, ControlEvent selection, text styles, size formatting, MaskedEdit
/// templates, and the Win32 window each control is created as (msi_ui.hpp).

#include "winapi/system/msi/msi_ui.hpp"
#include "winapi/system/msi/msi_package.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"

#include <algorithm>
#include <cstdlib>

namespace vitreo::winapi::msi {

namespace {

/// A cell of @p row by column name ("" when the column is absent).
std::string cell(const Table& t, const std::vector<std::string>& row, std::string_view column) {
    const auto i = t.column_index(column);
    if (!i || *i >= row.size()) return {};
    return row[*i];
}

i32 to_int(const std::string& s, i32 fallback = 0) {
    if (s.empty()) return fallback;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    return (end != nullptr && end != s.c_str()) ? static_cast<i32>(v) : fallback;
}

std::optional<i32> to_opt_int(const std::string& s) {
    if (s.empty()) return std::nullopt;
    return to_int(s);
}

std::u16string to_u16(const std::string& utf8) {
    const std::vector<u16> w = utf8_to_utf16(utf8);
    std::u16string out(w.size(), u'\0');
    for (usize i = 0; i < w.size(); ++i) out[i] = static_cast<char16_t>(w[i]);
    while (!out.empty() && out.back() == u'\0') out.pop_back();
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

// Win32 window styles (WinUser.h / CommCtrl.h / RichEdit.h). Each control's
// combination is the one Windows 11 creates (msi_ui_layout*.expected).
constexpr u32 WS_CHILD = 0x40000000, WS_VISIBLE = 0x10000000, WS_DISABLED = 0x08000000,
              WS_CLIPSIBLINGS = 0x04000000, WS_CLIPCHILDREN = 0x02000000, WS_CAPTION = 0x00C00000,
              WS_BORDER = 0x00800000, WS_SYSMENU = 0x00080000, WS_MINIMIZEBOX = 0x00020000,
              WS_GROUP = 0x00020000, WS_TABSTOP = 0x00010000;
constexpr u32 WS_EX_TRANSPARENT = 0x00000020, WS_EX_WINDOWEDGE = 0x00000100, WS_EX_CLIENTEDGE = 0x00000200,
              WS_EX_CONTROLPARENT = 0x00010000, WS_EX_STATICEDGE = 0x00020000, WS_EX_APPWINDOW = 0x00040000;
constexpr u32 BS_CHECKBOX = 0x2, BS_RADIOBUTTON = 0x4, BS_GROUPBOX = 0x7, BS_OWNERDRAW = 0xB,
              BS_DEFPUSHBUTTON = 0x1, BS_ICON = 0x40, BS_BITMAP = 0x80, BS_PUSHLIKE = 0x1000, BS_MULTILINE = 0x2000;
constexpr u32 SS_ICON = 0x3, SS_BITMAP = 0xE, SS_ETCHEDHORZ = 0x10, SS_NOPREFIX = 0x80, SS_CENTERIMAGE = 0x200,
              SS_SUNKEN = 0x1000, SS_LEFTNOWORDWRAP = 0xC, SS_ENDELLIPSIS = 0x4000;
constexpr u32 ES_MULTILINE = 0x4, ES_UPPERCASE = 0x8, ES_PASSWORD = 0x20, ES_AUTOVSCROLL = 0x40,
              ES_AUTOHSCROLL = 0x80, ES_READONLY = 0x800, ES_NUMBER = 0x2000;
constexpr u32 CBS_DROPDOWN = 0x2, CBS_DROPDOWNLIST = 0x3, CBS_OWNERDRAWFIXED = 0x10, CBS_AUTOHSCROLL = 0x40,
              CBS_SORT = 0x100, CBS_HASSTRINGS = 0x200;
constexpr u32 LBS_NOTIFY = 0x1, LBS_SORT = 0x2;

}  // namespace

// ---- Text styles ------------------------------------------------------------------------

FontSpec font_from_style(const TextStyleRow& style, i32 dpi) {
    FontSpec f;
    f.face = to_u16(style.face);
    // -MulDiv(points, dpi, 72): Tahoma 8 -> -11, 10 -> -13, 12 -> -16 at 96 DPI (probe).
    const i64 num = static_cast<i64>(style.size) * dpi;
    f.height = -static_cast<i32>((num + 36) / 72);
    f.weight = (style.bits & sdk::msidbTextStyleStyleBitsBold) ? 700 : 400;
    f.italic = (style.bits & sdk::msidbTextStyleStyleBitsItalic) != 0;
    f.underline = (style.bits & sdk::msidbTextStyleStyleBitsUnderline) != 0;
    f.strike = (style.bits & sdk::msidbTextStyleStyleBitsStrike) != 0;
    return f;
}

StyledText split_styled_text(const std::string& cell_text, const std::map<std::string, TextStyleRow>& styles) {
    StyledText out;
    usize i = 0;
    // Leading {\Style} / {&Style} groups name a font; {n} limits the length.
    // A group that is neither is ordinary text and ends the prefix.
    while (i < cell_text.size() && cell_text[i] == '{') {
        const usize close = cell_text.find('}', i);
        if (close == std::string::npos) break;
        const std::string inner = cell_text.substr(i + 1, close - i - 1);
        if (!inner.empty() && (inner[0] == '\\' || inner[0] == '&')) {
            const std::string name = inner.substr(1);
            if (styles.contains(name)) {
                out.style = name;
                i = close + 1;
                continue;
            }
            break;
        }
        if (!inner.empty() && !out.limit && !out.bad_limit) {
            bool digits = true;
            for (char c : inner) digits = digits && (c >= '0' && c <= '9');
            if (digits) {
                out.limit = std::strtoll(inner.c_str(), nullptr, 10);
                i = close + 1;
                continue;
            }
            // "A negative or non-numeric value will generate an error"
            // (Learn ComboBox control) — only a group that looks like a
            // limit: a sign or digits followed by something else.
            if (inner[0] == '-' || (inner[0] >= '0' && inner[0] <= '9')) {
                out.bad_limit = true;
                i = close + 1;
                continue;
            }
        }
        break;
    }
    out.text = cell_text.substr(i);
    return out;
}

std::optional<std::string> format_size_text(i64 units_512, const std::map<std::string, std::string>& ui_text,
                                            std::string* missing_key) {
    // Probe (D2 dialog): 0 -> "0KB", 1 -> "512B", 3 -> "1KB", 20479 ->
    // "10239KB", 20480 -> "10MB", 20971520 -> "10GB", -4 -> "-2KB": KB is
    // units/2 truncated; the next unit is used from 10 of it on; no space
    // between number and unit text.
    auto unit = [&](const char* key) -> std::optional<std::string> {
        auto it = ui_text.find(key);
        if (it == ui_text.end() && std::string_view(key) == "bytes") it = ui_text.find("Bytes");
        if (it == ui_text.end()) {
            if (missing_key) *missing_key = key;
            return std::nullopt;
        }
        return it->second;
    };
    const i64 kb = units_512 / 2;
    if (kb == 0 && units_512 != 0) {
        auto u = unit("bytes");
        if (!u) return std::nullopt;
        return std::to_string(units_512 * 512) + *u;
    }
    if (kb > -10240 && kb < 10240) {
        auto u = unit("KB");
        if (!u) return std::nullopt;
        return std::to_string(kb) + *u;
    }
    const i64 mb = kb / 1024;
    if (mb > -10240 && mb < 10240) {
        auto u = unit("MB");
        if (!u) return std::nullopt;
        return std::to_string(mb) + *u;
    }
    auto u = unit("GB");
    if (!u) return std::nullopt;
    return std::to_string(mb / 1024) + *u;
}

MaskTemplate parse_mask(const std::string& mask) {
    MaskTemplate t;
    usize begin = 0, end = mask.size();
    const usize lt = mask.find('<');
    if (lt != std::string::npos) {
        t.hidden_prefix = mask.substr(0, lt);
        begin = lt + 1;
        const usize gt = mask.find('>', begin);
        if (gt != std::string::npos) {
            end = gt;
            t.hidden_suffix = mask.substr(gt + 1);
        }
    }
    using K = MaskSegment::Kind;
    auto kind_of = [](char c) -> std::optional<K> {
        switch (c) {
            case '#': return K::Digit;
            case '%': return K::AltDigit;
            case '&': return K::AnyChar;
            case '^': case '?': case '`': return K::AltChar;
            default: return std::nullopt;
        }
    };
    for (usize i = begin; i < end; ++i) {
        const char c = mask[i];
        if (c == '=' && !t.segments.empty() && t.segments.back().kind != K::Literal) {
            // "creates one more input position of the same type as the
            // preceding positions and terminates the field with a '-'".
            t.segments.back().text += c;
            t.segments.back().length += 1;
            t.segments.push_back({K::Literal, "-", 0});
            continue;
        }
        const auto k = kind_of(c);
        if (k) {
            if (!t.segments.empty() && t.segments.back().kind == *k && t.segments.back().text.back() != '=') {
                t.segments.back().text += c;
                t.segments.back().length += 1;
            } else {
                t.segments.push_back({*k, std::string(1, c), 1});
            }
            continue;
        }
        // '@' (random digit) is not meant for the visible part; '_' is a
        // literal underscore; any other character is a literal constant.
        if (!t.segments.empty() && t.segments.back().kind == K::Literal) t.segments.back().text += c;
        else t.segments.push_back({K::Literal, std::string(1, c), 0});
    }
    return t;
}

// ---- Loading the UI tables -------------------------------------------------------------------

const ControlRow* DialogModel::control(const std::string& name) const {
    for (const ControlRow& c : controls)
        if (c.name == name) return &c;
    return nullptr;
}

std::map<std::string, TextStyleRow> load_text_styles(Package& package) {
    std::map<std::string, TextStyleRow> out;
    const Table* t = package.table("TextStyle");
    if (t == nullptr) return out;
    for (const auto& row : t->rows) {
        TextStyleRow s;
        s.face = cell(*t, row, "FaceName");
        s.size = to_int(cell(*t, row, "Size"));
        if (const std::string c = cell(*t, row, "Color"); !c.empty()) s.color = static_cast<u32>(to_int(c));
        s.bits = static_cast<u32>(to_int(cell(*t, row, "StyleBits")));
        out[cell(*t, row, "TextStyle")] = s;
    }
    return out;
}

std::map<std::string, std::string> load_ui_text(Package& package) {
    std::map<std::string, std::string> out;
    const Table* t = package.table("UIText");
    if (t == nullptr) return out;
    for (const auto& row : t->rows) out[cell(*t, row, "Key")] = cell(*t, row, "Text");
    return out;
}

std::vector<ListItem> load_list_items(Package& package, const std::string& table, const std::string& property) {
    std::vector<ListItem> out;
    const Table* t = package.table(table);
    if (t == nullptr) return out;
    for (const auto& row : t->rows) {
        if (cell(*t, row, "Property") != property) continue;
        ListItem it;
        it.order = to_int(cell(*t, row, "Order"));
        it.value = cell(*t, row, "Value");
        it.text = cell(*t, row, "Text");
        it.x = to_int(cell(*t, row, "X"));
        it.y = to_int(cell(*t, row, "Y"));
        it.width = to_int(cell(*t, row, "Width"));
        it.height = to_int(cell(*t, row, "Height"));
        it.help = cell(*t, row, "Help");
        it.binary = cell(*t, row, "Binary_");
        out.push_back(std::move(it));
    }
    std::stable_sort(out.begin(), out.end(), [](const ListItem& a, const ListItem& b) { return a.order < b.order; });
    return out;
}

namespace {

ControlRow read_control_row(const Table& t, const std::vector<std::string>& row, const char* name_column) {
    ControlRow c;
    c.name = cell(t, row, name_column);
    c.type = cell(t, row, "Type");
    c.x = to_int(cell(t, row, "X"));
    c.y = to_int(cell(t, row, "Y"));
    c.width = to_int(cell(t, row, "Width"));
    c.height = to_int(cell(t, row, "Height"));
    c.attributes = static_cast<u32>(to_int(cell(t, row, "Attributes")));
    c.property = cell(t, row, "Property");
    c.text = cell(t, row, "Text");
    c.next = cell(t, row, "Control_Next");
    c.help = cell(t, row, "Help");
    return c;
}

}  // namespace

std::vector<ControlRow> load_billboard_controls(Package& package, const std::string& billboard) {
    std::vector<ControlRow> out;
    const Table* t = package.table("BBControl");
    if (t == nullptr) return out;
    for (const auto& row : t->rows) {
        if (cell(*t, row, "Billboard_") != billboard) continue;
        out.push_back(read_control_row(*t, row, "BBControl"));
    }
    return out;
}

std::optional<DialogModel> load_dialog(Package& package, const std::string& name) {
    const Table* d = package.table("Dialog");
    if (d == nullptr) return std::nullopt;
    DialogModel m;
    bool found = false;
    for (const auto& row : d->rows) {
        if (cell(*d, row, "Dialog") != name) continue;
        m.row.name = name;
        m.row.hcenter = to_int(cell(*d, row, "HCentering"), 50);
        m.row.vcenter = to_int(cell(*d, row, "VCentering"), 50);
        m.row.width = to_int(cell(*d, row, "Width"));
        m.row.height = to_int(cell(*d, row, "Height"));
        m.row.attributes = static_cast<u32>(to_int(cell(*d, row, "Attributes")));
        m.row.title = cell(*d, row, "Title");
        m.row.first = cell(*d, row, "Control_First");
        m.row.default_control = cell(*d, row, "Control_Default");
        m.row.cancel = cell(*d, row, "Control_Cancel");
        found = true;
        break;
    }
    if (!found) return std::nullopt;
    if (const Table* t = package.table("Control")) {
        for (const auto& row : t->rows)
            if (cell(*t, row, "Dialog_") == name) m.controls.push_back(read_control_row(*t, row, "Control"));
    }
    if (const Table* t = package.table("ControlEvent")) {
        for (const auto& row : t->rows) {
            if (cell(*t, row, "Dialog_") != name) continue;
            ControlEventRow e;
            e.event = cell(*t, row, "Event");
            e.argument = cell(*t, row, "Argument");
            e.condition = cell(*t, row, "Condition");
            e.ordering = to_opt_int(cell(*t, row, "Ordering"));
            m.events[cell(*t, row, "Control_")].push_back(std::move(e));
        }
    }
    if (const Table* t = package.table("ControlCondition")) {
        for (const auto& row : t->rows) {
            if (cell(*t, row, "Dialog_") != name) continue;
            m.conditions[cell(*t, row, "Control_")].emplace_back(cell(*t, row, "Action"), cell(*t, row, "Condition"));
        }
    }
    if (const Table* t = package.table("EventMapping")) {
        for (const auto& row : t->rows) {
            if (cell(*t, row, "Dialog_") != name) continue;
            m.subscriptions.push_back({cell(*t, row, "Control_"), cell(*t, row, "Event"), cell(*t, row, "Attribute")});
        }
    }
    return m;
}

std::vector<ControlEventRow> select_control_events(
    const std::vector<ControlEventRow>& rows, const std::function<std::optional<bool>(const std::string&)>& eval) {
    std::vector<ControlEventRow> sorted = rows;
    std::stable_sort(sorted.begin(), sorted.end(), [](const ControlEventRow& a, const ControlEventRow& b) {
        return a.ordering.value_or(0) < b.ordering.value_or(0);
    });
    auto blank = [](const std::string& c) {
        return std::all_of(c.begin(), c.end(), [](char ch) { return ch == ' ' || ch == '\t'; });
    };
    std::vector<ControlEventRow> chosen;
    for (const ControlEventRow& r : sorted) {
        if (blank(r.condition)) continue;
        if (eval(r.condition).value_or(false)) chosen.push_back(r);
    }
    if (chosen.empty()) {
        // "If none of the Condition fields ... evaluate to True, the installer
        // triggers the one event having a blank Condition field, and if more
        // than one ... the one with the largest value in the Ordering field."
        const ControlEventRow* best = nullptr;
        for (const ControlEventRow& r : sorted)
            if (blank(r.condition) && (best == nullptr || r.ordering.value_or(0) >= best->ordering.value_or(0))) best = &r;
        if (best != nullptr) chosen.push_back(*best);
    }
    // At most one NewDialog / SpawnDialog: the selected one with the largest Ordering.
    const ControlEventRow* last_dialog = nullptr;
    for (const ControlEventRow& r : chosen)
        if (r.event == "NewDialog" || r.event == "SpawnDialog") last_dialog = &r;
    if (last_dialog != nullptr) {
        const ControlEventRow keep = *last_dialog;
        std::vector<ControlEventRow> filtered;
        for (const ControlEventRow& r : chosen) {
            if (r.event == "NewDialog" || r.event == "SpawnDialog") {
                if (r.event == keep.event && r.argument == keep.argument && r.ordering == keep.ordering &&
                    r.condition == keep.condition)
                    filtered.push_back(r);
                continue;
            }
            filtered.push_back(r);
        }
        chosen = std::move(filtered);
    }
    return chosen;
}

// ---- Window layout ---------------------------------------------------------------------

DialogWindowSpec dialog_window_spec(const DialogRow& row, const UnitScale& scale) {
    DialogWindowSpec s;
    s.no_close = row.cancel.empty();
    s.class_name = s.no_close ? u"MsiDialogNoCloseClass" : u"MsiDialogCloseClass";
    // Probe: 0x14c80000 (visible once shown) / 0x00040100 for both classes.
    s.style = WS_CLIPSIBLINGS | WS_CAPTION | WS_SYSMENU;
    if (row.attributes & sdk::msidbDialogAttributesMinimize) s.style |= WS_MINIMIZEBOX;
    s.ex_style = WS_EX_APPWINDOW | WS_EX_WINDOWEDGE;
    s.client_w = scale.px(row.width);
    s.client_h = scale.px(row.height);
    s.hcenter = std::clamp(row.hcenter, 0, 100);
    s.vcenter = std::clamp(row.vcenter, 0, 100);
    return s;
}

std::optional<WindowSpec> control_window_spec(const ControlRow& row, const UnitScale& scale, bool is_default,
                                              i32 origin_x, i32 origin_y) {
    const std::string& type = row.type;
    const u32 a = row.attributes;
    WindowSpec s;
    s.rect = {scale.px(origin_x + row.x), scale.px(origin_y + row.y), scale.px(row.width), scale.px(row.height)};
    // Origin offsets convert summed units (BBControl). Width/height alone.
    u32 style = WS_CHILD;
    if (a & sdk::msidbControlAttributesVisible) style |= WS_VISIBLE;
    if (!(a & sdk::msidbControlAttributesEnabled)) style |= WS_DISABLED;
    u32 ex = (a & sdk::msidbControlAttributesSunken) ? WS_EX_CLIENTEDGE : 0;
    if (iequals(type, "PushButton")) {
        s.class_name = u"Button";
        style |= WS_TABSTOP | WS_GROUP | BS_MULTILINE;
        if (is_default) style |= BS_DEFPUSHBUTTON;
        if (a & sdk::msidbControlAttributesBitmap) style |= BS_BITMAP;
        else if (a & sdk::msidbControlAttributesIcon) style |= BS_ICON;
    } else if (iequals(type, "Text")) {
        s.class_name = u"Static";
        style |= WS_GROUP;
        if (a & sdk::msidbControlAttributesNoPrefix) style |= SS_NOPREFIX;
        if (a & sdk::msidbControlAttributesNoWrap) style |= SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS;
        if (a & sdk::msidbControlAttributesTransparent) ex |= WS_EX_TRANSPARENT;
    } else if (iequals(type, "Edit") || iequals(type, "PathEdit")) {
        style |= WS_BORDER | WS_TABSTOP | WS_GROUP | ES_AUTOHSCROLL;
        if (iequals(type, "Edit") && (a & sdk::msidbControlAttributesPasswordInput)) {
            s.class_name = u"Edit";
            style |= ES_PASSWORD;
        } else {
            s.class_name = u"RichEdit20W";
            style |= ES_UPPERCASE;  // measured on every RichEdit field (0x88 / 0xcc)
            if (iequals(type, "Edit") && (a & sdk::msidbControlAttributesMultiline)) style |= ES_MULTILINE | ES_AUTOVSCROLL;
        }
    } else if (iequals(type, "MaskedEdit")) {
        // A container; its segment windows are laid out by the dialog
        // (mask_segment_specs) because they depend on the font's metrics.
        s.class_name = u"Static";
        style |= WS_GROUP;
        ex |= WS_EX_CONTROLPARENT;
    } else if (iequals(type, "CheckBox")) {
        s.class_name = u"Button";
        style |= WS_TABSTOP | WS_GROUP | BS_MULTILINE | BS_CHECKBOX;
        if (a & sdk::msidbControlAttributesPushLike) style |= BS_PUSHLIKE;
        if (a & sdk::msidbControlAttributesBitmap) style |= BS_BITMAP;
        else if (a & sdk::msidbControlAttributesIcon) style |= BS_ICON;
    } else if (iequals(type, "RadioButtonGroup")) {
        s.class_name = u"Button";
        style |= WS_CLIPCHILDREN | WS_GROUP | ((a & sdk::msidbControlAttributesHasBorder) ? BS_GROUPBOX : BS_OWNERDRAW);
        ex |= WS_EX_CONTROLPARENT;
    } else if (iequals(type, "ComboBox")) {
        s.class_name = u"ComboBox";
        ex &= ~WS_EX_CLIENTEDGE;  // probe: Sunken combo boxes have no client edge
        style |= WS_TABSTOP | WS_GROUP | CBS_AUTOHSCROLL | CBS_HASSTRINGS;
        if (!(a & sdk::msidbControlAttributesSorted)) style |= CBS_SORT;
        style |= (a & sdk::msidbControlAttributesComboList) ? CBS_DROPDOWNLIST : CBS_DROPDOWN;
    } else if (iequals(type, "ListBox")) {
        s.class_name = u"ListBox";
        style |= WS_BORDER | WS_TABSTOP | WS_GROUP | LBS_NOTIFY;
        if (!(a & sdk::msidbControlAttributesSorted)) style |= LBS_SORT;
    } else if (iequals(type, "ListView")) {
        s.class_name = u"SysListView32";
        // Probe: 0x5093405d (LVS_REPORT|SINGLESEL|SHOWSELALWAYS|SORTASCENDING|
        // SHAREIMAGELISTS|NOCOLUMNHEADER, WS_HSCROLL, WS_BORDER).
        style |= WS_BORDER | 0x00100000 | WS_TABSTOP | WS_GROUP | 0x404D;
        if (!(a & sdk::msidbControlAttributesSorted)) style |= 0x10;
    } else if (iequals(type, "DirectoryCombo") || iequals(type, "VolumeSelectCombo")) {
        s.class_name = u"ComboBox";
        ex &= ~WS_EX_CLIENTEDGE;
        style |= WS_TABSTOP | WS_GROUP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_SORT | CBS_HASSTRINGS;
    } else if (iequals(type, "DirectoryList")) {
        s.class_name = u"SysListView32";
        style |= WS_BORDER | WS_TABSTOP | WS_GROUP | 0x035F;
    } else if (iequals(type, "VolumeCostList")) {
        s.class_name = u"SysListView32";
        style |= WS_BORDER | WS_TABSTOP | WS_GROUP | 0x0141;
    } else if (iequals(type, "SelectionTree")) {
        s.class_name = u"SysTreeView32";
        // TVS_HASBUTTONS|HASLINES|LINESATROOT|DISABLEDRAGDROP|SHOWSELALWAYS (0x37).
        style |= WS_BORDER | WS_TABSTOP | WS_GROUP | 0x37;
    } else if (iequals(type, "ProgressBar")) {
        s.class_name = u"msctls_progress32";
        style |= WS_GROUP;
    } else if (iequals(type, "Bitmap")) {
        s.class_name = u"Static";
        style |= WS_GROUP | SS_BITMAP | SS_CENTERIMAGE;
        s.has_font = false;
    } else if (iequals(type, "Icon")) {
        s.class_name = u"Static";
        style |= WS_GROUP | SS_ICON | SS_CENTERIMAGE;
        s.has_font = false;
    } else if (iequals(type, "Line")) {
        s.class_name = u"Static";
        style |= WS_GROUP | SS_ETCHEDHORZ | SS_SUNKEN;
        ex = WS_EX_STATICEDGE;
        // Probe: a Line is 2 px high whatever its Height, and 2 px narrower
        // than its Width (300 units -> 398 px).
        s.rect.h = 2;
        s.rect.w = std::max(0, s.rect.w - 2);
        s.has_font = false;
    } else if (iequals(type, "GroupBox")) {
        s.class_name = u"Button";
        style |= WS_GROUP | BS_GROUPBOX;
    } else if (iequals(type, "ScrollableText")) {
        s.class_name = u"RichEdit20W";
        style |= WS_TABSTOP | WS_GROUP | ES_READONLY | ES_AUTOVSCROLL | ES_MULTILINE | ES_UPPERCASE;
        s.has_font = false;
    } else if (iequals(type, "Hyperlink")) {
        s.class_name = u"SysLink";
        style |= WS_TABSTOP | WS_GROUP;
    } else if (iequals(type, "Billboard")) {
        return std::nullopt;  // "This control has no window of its own."
    } else {
        return std::nullopt;  // not a control type Windows Installer defines
    }
    s.style = style;
    s.ex_style = ex;
    s.control_type = type;
    return s;
}

}  // namespace vitreo::winapi::msi
