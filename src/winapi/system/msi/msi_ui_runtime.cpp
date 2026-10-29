// SPDX-License-Identifier: LGPL-2.1-only
/// @file msi_ui_runtime.cpp
/// @brief Windows Installer internal UI at run time: live dialogs, the
/// ControlEvent engine, property binding, subscriptions (EventMapping),
/// progress, error dialogs, the basic-UI progress window, and UI preview
/// (InstallerUi, msi_ui.hpp).
///
/// Semantics: Learn ControlEvent / ControlCondition / EventMapping tables,
/// every ControlEvent and control page, User Interface Levels, Error Dialog,
/// FilesInUse / MsiRMFilesInUse dialogs, MsiProcessMessage and "Parsing
/// Windows Installer Messages" (record fields of each message type).
/// Text Vitreo shows that no package table supplies (the basic progress
/// window, the end dialog of INSTALLUILEVEL_ENDDIALOG) is Vitreo's own.

#include "winapi/system/msi/msi_ui.hpp"
#include "winapi/system/msi/msi_engine.hpp"
#include "winapi/system/msi/msi_features.hpp"
#include "winapi/system/msi/msi_package.hpp"
#include "winapi/system/msi/msi.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace vitreo::winapi::msi {

namespace {

constexpr i32 IDOK = 1, IDCANCEL = 2, IDABORT = 3, IDRETRY = 4, IDIGNORE = 5, IDYES = 6, IDNO = 7;
constexpr u32 MB_TYPEMASK = 0x0F, MB_ICONMASK = 0xF0, MB_DEFMASK = 0xF00;
constexpr u32 MB_OK = 0, MB_OKCANCEL = 1, MB_ABORTRETRYIGNORE = 2, MB_YESNOCANCEL = 3, MB_YESNO = 4,
              MB_RETRYCANCEL = 5;
constexpr u32 IDI_ERROR = 32513, IDI_WARNING = 32515, IDI_INFORMATION = 32516;
// Win32 styles of the windows created here (as in msi_ui_dialog.cpp).
constexpr u32 WS_CHILD = 0x40000000, WS_VISIBLE = 0x10000000, WS_GROUP = 0x00020000, WS_TABSTOP = 0x00010000;
constexpr u32 BS_RADIOBUTTON = 0x4, BS_MULTILINE = 0x2000, BS_ICON = 0x40, BS_BITMAP = 0x80;
constexpr u32 ES_AUTOHSCROLL = 0x80, ES_NUMBER = 0x2000, WS_EX_TRANSPARENT = 0x20;

std::u16string wide(const std::string& utf8) {
    const std::vector<u16> w = utf8_to_utf16(utf8);
    std::u16string out(w.size(), u'\0');
    for (usize i = 0; i < w.size(); ++i) out[i] = static_cast<char16_t>(w[i]);
    while (!out.empty() && out.back() == u'\0') out.pop_back();
    return out;
}

std::string field_text(const Record& r, u32 i) {
    if (i >= r.fields.size()) return {};
    const Record::Field& f = r.fields[i];
    switch (f.kind) {
        case Record::Kind::Integer: return std::to_string(f.integer);
        case Record::Kind::String: {
            std::vector<u16> w(f.text.begin(), f.text.end());
            return utf16_to_utf8(w.data(), static_cast<int>(w.size()));
        }
        default: return {};
    }
}
i64 field_int(const Record& r, u32 i) {
    if (i >= r.fields.size()) return 0;
    const Record::Field& f = r.fields[i];
    if (f.kind == Record::Kind::Integer) return f.integer;
    if (f.kind == Record::Kind::String) return std::strtoll(field_text(r, i).c_str(), nullptr, 10);
    return 0;
}
bool field_null(const Record& r, u32 i) { return i >= r.fields.size() || r.fields[i].kind == Record::Kind::Null; }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

bool is_int_text(const std::string& s) {
    if (s.empty()) return false;
    usize i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i == s.size()) return false;
    for (; i < s.size(); ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

/// Outcome of a dialog's event processing.
struct Outcome {
    enum class Kind : u8 { None, NewDialog, Return, Exit, Retry, Ignore, ErrorButton, Blocked };
    Kind kind{Kind::None};
    std::string dialog;  ///< NewDialog target
    i32 button{0};       ///< ErrorButton IDOK..IDNO
};

struct LiveControl {
    ControlRow row;
    u64 window{0};
    std::vector<u64> children;         ///< radio buttons / mask segment edits
    std::vector<MaskSegment> segments; ///< MaskedEdit editable segments, in child order (edits only)
    MaskTemplate mask;
    std::string property;              ///< resolved (Indirect: the property named by Property)
    std::string style;                 ///< TextStyle in effect
    std::optional<i64> limit;
    std::string text_template;         ///< Text after the style prefixes, unformatted
    std::vector<ListItem> items;
    std::string checkbox_value;
    bool visible{false}, enabled{false};
    bool from_billboard{false};
    // SelectionTree / DirectoryList state
    std::string selected;
    std::string new_folder;            ///< DirectoryListNew's folder, created once
    // ProgressBar
    i64 position{0}, range{1024};
};

struct LiveDialog {
    DialogModel model;
    u64 window{0};
    std::vector<LiveControl> controls;
    std::string default_control;
    /// Reset ControlEvent: property values at creation, of the properties
    /// the dialog's controls change.
    std::map<std::string, std::optional<std::string>> snapshot;
    std::optional<std::string> wait_condition;  ///< SpawnWaitDialog
    bool preview{false};
    std::string billboard_host;   ///< Billboard control showing a billboard
    std::string billboard;
    std::vector<u64> billboard_windows;

    LiveControl* find(const std::string& name) {
        for (LiveControl& c : controls)
            if (c.row.name == name) return &c;
        return nullptr;
    }
    LiveControl* find_window(u64 w) {
        for (LiveControl& c : controls) {
            if (c.window == w) return &c;
            for (u64 ch : c.children)
                if (ch == w) return &c;
        }
        return nullptr;
    }
};

/// The progress state machine of INSTALLMESSAGE_PROGRESS (Learn "Parsing
/// Windows Installer Messages").
struct Progress {
    bool started{false};
    i64 total{0}, position{0};
    bool forward{true};
    bool script{false};
    i64 per_action_data{0};
    std::chrono::steady_clock::time_point reset_at{};
    std::string action;
};

}  // namespace

struct InstallerUi::Impl {
    MsiManager& mgr;
    std::shared_ptr<WindowSystem> ws;
    UiHooks hooks;
    u32 level;
    std::vector<std::unique_ptr<LiveDialog>> stack;   ///< modal wizard dialog and spawned children
    std::unique_ptr<LiveDialog> modeless;              ///< the modeless dialog the UI sequence left up
    Progress progress;
    std::string caption;
    bool cancel_shown{true};
    bool cancel_pending{false};
    struct Basic {
        u64 dialog{0}, text{0}, bar{0}, cancel{0};
    };
    std::optional<Basic> basic;
    std::map<std::string, TextStyleRow> styles;
    std::map<std::string, std::string> ui_text;
    const Package* cached_for{nullptr};

    Impl(MsiManager& m, std::shared_ptr<WindowSystem> w, UiHooks h, u32 l)
        : mgr(m), ws(std::move(w)), hooks(std::move(h)), level(l) {}

    [[nodiscard]] u32 base_level() const { return level & ~kUiLevelModifiers; }
    [[nodiscard]] UnitScale scale() const { return UnitScale{ws ? ws->system_text_height() : 16}; }

    void cache(Session& s) {
        if (s.package() == cached_for || s.package() == nullptr) return;
        styles = load_text_styles(*s.package());
        ui_text = load_ui_text(*s.package());
        cached_for = s.package();
    }

    std::optional<bool> eval(Session& s, const std::string& c) { return s.evaluate_condition(c); }

    // -- fonts and text ------------------------------------------------------------------

    std::optional<FontSpec> font_for(Session& s, const std::string& style) {
        std::string name = style.empty() ? s.property("DefaultUIFont") : style;
        auto it = styles.find(name);
        if (it == styles.end()) return std::nullopt;  // the system font (DefaultUIFont remarks)
        return font_from_style(it->second, ws->dpi());
    }
    std::optional<u32> color_for(const std::string& style, Session& s) {
        std::string name = style.empty() ? s.property("DefaultUIFont") : style;
        auto it = styles.find(name);
        return it == styles.end() ? std::nullopt : it->second.color;
    }

    std::string control_text(Session& s, const LiveControl& c) {
        std::string text = s.format(c.text_template);
        if (iequals(c.row.type, "Text") && (c.row.attributes & sdk::msidbControlAttributesFormatSize)) {
            std::string missing;
            auto sized = format_size_text(std::strtoll(text.c_str(), nullptr, 10), ui_text, &missing);
            // Windows shows its own diagnostic text here; Vitreo writes its own.
            text = sized ? *sized : (text + " (UIText " + missing + " missing)");
        }
        return text;
    }

    const std::vector<u8>* binary(Session& s, const std::string& key) {
        return s.package() != nullptr ? s.package()->stream("Binary." + key) : nullptr;
    }

    static i32 icon_size(u32 a) {
        switch (a & sdk::msidbControlAttributesIconSize48) {
            case sdk::msidbControlAttributesIconSize16: return 16;
            case sdk::msidbControlAttributesIconSize32: return 32;
            case sdk::msidbControlAttributesIconSize48: return 48;
            default: return 0;
        }
    }

    // -- property binding ----------------------------------------------------------------

    std::string resolve_property(Session& s, const ControlRow& r) {
        if (r.property.empty()) return {};
        if (r.attributes & sdk::msidbControlAttributesIndirect) return s.property(r.property);
        return r.property;
    }

    void set_bound_property(Session& s, LiveDialog& d, LiveControl& c, const std::string& value) {
        if (c.property.empty()) return;
        if (!d.snapshot.contains(c.property)) {
            d.snapshot[c.property] = s.has_property(c.property) ? std::optional(s.property(c.property)) : std::nullopt;
        }
        // Integer controls keep only integer values (Control Attributes: Integer).
        if ((c.row.attributes & sdk::msidbControlAttributesInteger) && !value.empty() && !is_int_text(value)) return;
        s.set_property(c.property, value);
    }

    // -- creating windows ----------------------------------------------------------------

    void apply_font(Session& s, u64 w, const LiveControl& c, bool text_colour) {
        auto f = font_for(s, c.style);
        if (f) ws->set_font(w, *f, text_colour ? color_for(c.style, s) : std::nullopt);
    }

    void create_control(Session& s, LiveDialog& d, LiveControl& c, i32 ox = 0, i32 oy = 0) {
        const UnitScale sc = scale();
        const bool is_default = c.row.name == d.default_control;
        auto spec = control_window_spec(c.row, sc, is_default, ox, oy);
        StyledText st = split_styled_text(c.row.text, styles);
        c.style = st.style;
        c.limit = st.limit;
        c.text_template = st.text;
        c.property = resolve_property(s, c.row);
        c.visible = (c.row.attributes & sdk::msidbControlAttributesVisible) != 0;
        c.enabled = (c.row.attributes & sdk::msidbControlAttributesEnabled) != 0;
        if (!spec) return;  // Billboard: no window of its own
        const std::string& t = c.row.type;
        if (iequals(t, "Text") || iequals(t, "PushButton") || iequals(t, "CheckBox") || iequals(t, "GroupBox") ||
            iequals(t, "RadioButtonGroup") || iequals(t, "Hyperlink") || iequals(t, "Bitmap") || iequals(t, "Icon") ||
            iequals(t, "Line")) {
            spec->text = wide(iequals(t, "Bitmap") || iequals(t, "Icon") ? s.format(c.text_template) : control_text(s, c));
        }
        c.window = ws->create_control(d.window, *spec);
        if (c.window == 0) return;
        const u32 a = c.row.attributes;
        if (spec->has_font) apply_font(s, c.window, c, iequals(t, "Text"));
        if (iequals(t, "PushButton") || iequals(t, "CheckBox")) {
            if (a & (sdk::msidbControlAttributesBitmap | sdk::msidbControlAttributesIcon)) {
                if (const auto* b = binary(s, s.format(c.text_template)))
                    ws->set_image(c.window, *b, (a & sdk::msidbControlAttributesIcon) != 0, icon_size(a));
            }
        }
        if (iequals(t, "Bitmap") || iequals(t, "Icon")) {
            if (const auto* b = binary(s, s.format(c.text_template))) ws->set_image(c.window, *b, iequals(t, "Icon"), icon_size(a));
        }
        if (iequals(t, "ScrollableText")) ws->set_rich_text(c.window, c.text_template);
        if (iequals(t, "CheckBox")) {
            const auto v = s.package() ? load_list_items(*s.package(), "CheckBox", c.property) : std::vector<ListItem>{};
            // Learn CheckBox table: the Value column (formatted at creation),
            // else the property's original value, else "1".
            c.checkbox_value = !v.empty() && !v.front().value.empty() ? s.format(v.front().value) : s.property(c.property);
            if (c.checkbox_value.empty()) c.checkbox_value = "1";
            ws->set_checked(c.window, !s.property(c.property).empty());
        }
        if (iequals(t, "RadioButtonGroup")) create_radio_buttons(s, d, c);
        if (iequals(t, "ListBox") || iequals(t, "ComboBox") || iequals(t, "ListView")) {
            if (s.package()) c.items = load_list_items(*s.package(), t, c.property);
            fill_items(s, c);
        }
        if (iequals(t, "Edit") || iequals(t, "PathEdit") || iequals(t, "ComboBox")) {
            if (!iequals(t, "ComboBox") || !(a & sdk::msidbControlAttributesComboList)) ws->set_text(c.window, wide(s.property(c.property)));
        }
        if (iequals(t, "MaskedEdit")) create_mask_segments(s, c, *spec);
        if (iequals(t, "ProgressBar")) ws->set_progress(c.window, 0, static_cast<u32>(c.range));
        if (iequals(t, "SelectionTree")) fill_tree(s, d, c);
        if (iequals(t, "DirectoryList") || iequals(t, "DirectoryCombo")) fill_directory(s, c);
        if (iequals(t, "VolumeSelectCombo") || iequals(t, "VolumeCostList")) fill_volumes(s, c);
    }

    void create_radio_buttons(Session& s, LiveDialog& d, LiveControl& c) {
        (void)d;
        if (!s.package()) return;
        c.items = load_list_items(*s.package(), "RadioButton", c.property);
        const UnitScale sc = scale();
        const std::string current = s.property(c.property);
        bool first = true;
        for (ListItem& it : c.items) {
            it.value = s.format(it.value);  // formatted at creation (RadioButton table remarks)
            WindowSpec b;
            b.class_name = u"Button";
            const bool image = (c.row.attributes & (sdk::msidbControlAttributesBitmap | sdk::msidbControlAttributesIcon)) != 0;
            const bool selected = it.value == current;
            // Probe: 0x50022004 for the first, 0x50002004 for the rest; the
            // selected one is the group's tab stop (RadioButton table remarks).
            b.style = WS_CHILD | WS_VISIBLE | BS_RADIOBUTTON | BS_MULTILINE | (first ? WS_GROUP : 0) | (selected ? WS_TABSTOP : 0);
            if (image) b.style |= (c.row.attributes & sdk::msidbControlAttributesBitmap) ? BS_BITMAP : BS_ICON;
            b.rect = {sc.px(it.x), sc.px(it.y), sc.px(it.width), sc.px(it.height)};
            // A title's {\Style}/{&Style} prefix is not shown; that style sets
            // the button's font (probe msi_ui_layout: '{\Big}Delta' -> 'Delta'
            // in Big's font).
            const StyledText title = split_styled_text(it.text, styles);
            b.text = image ? u"" : wide(s.format(title.text));
            const u64 w = ws->create_control(c.window, b);
            if (w != 0) {
                if (!image && !title.style.empty()) {
                    if (auto f = font_for(s, title.style)) ws->set_font(w, *f, std::nullopt);
                } else {
                    apply_font(s, w, c, false);
                }
                if (image)
                    if (const auto* bin = binary(s, s.format(it.text)))
                        ws->set_image(w, *bin, (c.row.attributes & sdk::msidbControlAttributesIcon) != 0, icon_size(c.row.attributes));
                ws->set_checked(w, selected);
            }
            c.children.push_back(w);
            first = false;
        }
    }

    void fill_items(Session& s, LiveControl& c) {
        std::vector<std::pair<std::string, std::string>> items;
        for (ListItem& it : c.items) {
            const std::string text = s.format(it.text);
            items.emplace_back(text.empty() ? it.value : text, it.value);
        }
        ws->set_items(c.window, items, s.property(c.property));
    }

    void create_mask_segments(Session& s, LiveControl& c, const WindowSpec& container) {
        c.mask = parse_mask(c.text_template);
        const FontSpec font = font_for(s, c.style).value_or(FontSpec{});
        // Probe layout (Tahoma 8 / 10), in the container's client
        // coordinates: each edit is (widest digit x positions) + 8 px wide
        // and the container's height, each literal its text width and
        // vertically centred; 2 px between windows.
        i32 widest = 0;
        for (char16_t ch = u'0'; ch <= u'9'; ++ch) widest = std::max(widest, ws->text_extent(font, std::u16string(1, ch)).first);
        const i32 text_h = ws->text_extent(font, u"0").second;
        i32 x = 0;
        const std::string value = s.property(c.property);
        usize pos = c.mask.hidden_prefix.size();
        for (const MaskSegment& seg : c.mask.segments) {
            WindowSpec w;
            if (seg.kind == MaskSegment::Kind::Literal) {
                const auto ext = ws->text_extent(font, wide(seg.text));
                w.class_name = u"Static";
                w.style = WS_CHILD | WS_VISIBLE;
                w.ex_style = WS_EX_TRANSPARENT;
                w.text = wide(seg.text);
                w.rect = {x, (container.rect.h - text_h) / 2, ext.first, container.rect.h};
                x += ext.first + 2;
                pos += seg.text.size();
            } else {
                const bool digits = seg.kind == MaskSegment::Kind::Digit || seg.kind == MaskSegment::Kind::AltDigit;
                w.class_name = u"Edit";
                w.style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | (digits ? ES_NUMBER : 0);
                w.rect = {x, 0, widest * seg.length + 8, container.rect.h};
                const std::string part = pos < value.size() ? value.substr(pos, static_cast<usize>(seg.length)) : "";
                w.text = wide(part);
                x += w.rect.w + 2;
                pos += static_cast<usize>(seg.length);
            }
            const u64 h = ws->create_control(c.window, w);
            if (h != 0) ws->set_font(h, font, std::nullopt);
            if (seg.kind != MaskSegment::Kind::Literal) {
                c.children.push_back(h);
                c.segments.push_back(seg);
            }
        }
    }

    /// The property value a MaskedEdit's segment texts make: the hidden
    /// prefix, then each segment (literals as authored, '>' as '-' per the
    /// template rules), then the hidden suffix.
    std::string mask_value(const LiveControl& c, usize changed, const std::string& text, std::vector<std::string>& parts) {
        if (parts.size() < c.segments.size()) parts.resize(c.segments.size());
        if (changed < parts.size()) parts[changed] = text;
        std::string v = c.mask.hidden_prefix;
        usize edit = 0;
        for (const MaskSegment& seg : c.mask.segments) {
            if (seg.kind == MaskSegment::Kind::Literal) v += seg.text;
            else v += edit < parts.size() ? parts[edit++] : "";
        }
        if (!c.mask.hidden_suffix.empty()) v += "-" + c.mask.hidden_suffix;
        return v;
    }
    std::map<const LiveControl*, std::vector<std::string>> mask_parts;

    // -- SelectionTree ----------------------------------------------------------------------

    std::string ui(const std::string& key) {
        auto it = ui_text.find(key);
        return it == ui_text.end() ? std::string() : it->second;
    }

    void fill_tree(Session& s, LiveDialog& d, LiveControl& c) {
        std::vector<WindowSystem::TreeNode> nodes;
        FeatureModel& fm = feature_model(s);
        std::vector<const FeatureInfo*> shown;
        for (const std::string& name : fm.feature_order) {
            const FeatureInfo& f = fm.features.at(name);
            if (f.display == 0) continue;  // Feature.Display 0: not displayed
            shown.push_back(&f);
        }
        std::stable_sort(shown.begin(), shown.end(), [](const FeatureInfo* a, const FeatureInfo* b) {
            return (a->display >> 1) < (b->display >> 1);
        });
        for (const FeatureInfo* f : shown) {
            WindowSystem::TreeNode n;
            n.key = f->name;
            n.parent = f->parent;
            n.text = s.format(f->title);
            n.expanded = (f->display & 1) != 0;  // odd Display: expanded
            n.state_image = f->action != install_state::kUnknown ? f->action : f->installed;
            const u32 valid = feature_valid_states(s, f->name);
            auto add = [&](i32 st, const char* key) {
                if (valid & (1u << st)) n.menu.emplace_back(key, ui(key));
            };
            add(install_state::kLocal, "MenuLocal");
            add(install_state::kSource, "MenuNetwork");
            add(install_state::kAdvertised, "MenuAdvertise");
            add(install_state::kAbsent, "MenuAbsent");
            if (!f->children.empty()) {
                add(install_state::kLocal, "MenuAllLocal");
                add(install_state::kSource, "MenuAllNetwork");
            }
            nodes.push_back(std::move(n));
        }
        if (c.selected.empty() && !nodes.empty()) c.selected = nodes.front().key;
        ws->set_tree(c.window, nodes, c.selected);
        if (nodes.empty()) publish(s, d, "SelectionNoItems", "");
        else publish_selection(s, d, c);
    }

    static const char* state_word(i32 st) {
        switch (st) {
            case install_state::kLocal: return "Local";
            case install_state::kSource: return "Network";
            case install_state::kAdvertised: return "Advertise";
            default: return "Absent";
        }
    }

    void publish_selection(Session& s, LiveDialog& d, LiveControl& c) {
        const FeatureInfo* f = find_feature(s, c.selected);
        if (f == nullptr) return;
        const i32 action = f->request != install_state::kUnknown ? f->request : f->installed;
        const i32 installed = f->installed == install_state::kUnknown ? install_state::kAbsent : f->installed;
        publish(s, d, "SelectionDescription", s.format(f->description));
        // Sel<installed><action> (Learn SelectionTree: SelAbsentLocal ...).
        publish(s, d, "SelectionAction", ui(std::string("Sel") + state_word(installed) + state_word(action)));
        const bool path_on = !f->directory.empty() && action == install_state::kLocal;
        publish(s, d, "SelectionPathOn", path_on ? "1" : "0");
        publish(s, d, "SelectionPath", path_on ? target_path(s, f->directory) : ui("AbsentPath"));
        const i64 self = feature_cost(s, f->name, 0, action) / 512;
        const i64 all = feature_cost(s, f->name, 1, action) / 512;
        const i64 children_cost = all - self;
        std::string missing;
        auto size = [&](i64 v) { return format_size_text(v < 0 ? -v : v, ui_text, &missing).value_or(std::to_string(v)); };
        std::string text;
        Record r(4);
        auto set = [&](u32 i, const std::string& v) {
            r.fields[i].kind = Record::Kind::String;
            r.fields[i].text = wide(v);
        };
        set(1, size(self));
        if (f->children.empty()) {
            r.fields[0].kind = Record::Kind::String;
            r.fields[0].text = wide(ui(self >= 0 ? "SelChildCostPos" : "SelChildCostNeg"));
        } else {
            i32 installing = 0;
            for (const std::string& ch : f->children) {
                const FeatureInfo* cf = find_feature(s, ch);
                if (cf && (cf->request == install_state::kLocal || cf->request == install_state::kSource ||
                           cf->request == install_state::kAdvertised))
                    ++installing;
            }
            set(2, std::to_string(installing));
            set(3, std::to_string(f->children.size()));
            set(4, size(children_cost));
            std::string key = std::string("SelParentCost") + (self >= 0 ? "Pos" : "Neg") + (children_cost >= 0 ? "Pos" : "Neg");
            r.fields[0].kind = Record::Kind::String;
            r.fields[0].text = wide(ui(key));
        }
        publish(s, d, "SelectionSize", s.format_record(r));
        // The MsiSelectionTree* properties custom actions read (Learn
        // SelectionTree control, Windows Installer 3.0+).
        s.set_property("MsiSelectionTreeSelectedFeature", f->name);
        s.set_property("MsiSelectionTreeSelectedAction", std::to_string(action));
        s.set_property("MsiSelectonTreeChildrenCount", std::to_string(f->children.size()));
        s.set_property("MsiSelectionTreeSelectedCost", std::to_string(self));
        s.set_property("MsiSelectionTreeChildrenCost", std::to_string(children_cost));
        s.set_property("MsiSelectionTreeSelectedPath", path_on ? target_path(s, f->directory) : "");
        // The tree's property names the highlighted feature's configurable
        // directory, which a Browse dialog edits (SelectionBrowse).
        if (!c.property.empty() && !f->directory.empty()) s.set_property(c.property, f->directory);
    }

    void tree_pick(Session& s, LiveDialog& d, LiveControl& c, const std::string& value) {
        const usize tab = value.find('\t');
        if (tab == std::string::npos) return;
        const std::string feature = value.substr(0, tab), key = value.substr(tab + 1);
        i32 st = install_state::kAbsent;
        bool all = false;
        if (key == "MenuLocal") st = install_state::kLocal;
        else if (key == "MenuNetwork" || key == "MenuCD") st = install_state::kSource;
        else if (key == "MenuAdvertise") st = install_state::kAdvertised;
        else if (key == "MenuAllLocal") st = install_state::kLocal, all = true;
        else if (key == "MenuAllNetwork" || key == "MenuAllCD") st = install_state::kSource, all = true;
        std::function<void(const std::string&)> apply = [&](const std::string& f) {
            (void)set_feature_state(s, f, st);
            if (!all) return;
            if (const FeatureInfo* fi = find_feature(s, f))
                for (const std::string& ch : fi->children) apply(ch);
        };
        apply(feature);
        recompute_states(s);
        c.selected = feature;
        fill_tree(s, d, c);
    }

    // -- directories and volumes ----------------------------------------------------------

    static std::string parent_of(const std::string& path) {
        std::string p = path;
        while (!p.empty() && p.back() == '\\') p.pop_back();
        const usize slash = p.rfind('\\');
        if (slash == std::string::npos) return path;
        return p.substr(0, slash + 1);
    }
    static std::string with_slash(std::string p) {
        if (!p.empty() && p.back() != '\\') p += '\\';
        return p;
    }

    void fill_directory(Session& s, LiveControl& c) {
        const std::string path = with_slash(s.property(c.property));
        std::vector<std::pair<std::string, std::string>> items;
        if (iequals(c.row.type, "DirectoryList")) {
            if (hooks.list_folders)
                for (const std::string& f : hooks.list_folders(s, path)) items.emplace_back(f, f);
            ws->set_items(c.window, items, c.selected);
        } else {
            // DirectoryCombo: the path's chain of folders, then the volumes.
            std::string p = path;
            std::vector<std::string> chain;
            while (!p.empty()) {
                chain.push_back(p);
                const std::string up = parent_of(p);
                if (up == p) break;
                p = up;
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) items.emplace_back(*it, *it);
            if (hooks.list_volumes)
                for (const std::string& v : hooks.list_volumes(s))
                    if (std::none_of(items.begin(), items.end(), [&](const auto& i) { return iequals(i.second, v); }))
                        items.emplace_back(v, v);
            ws->set_items(c.window, items, path);
        }
    }

    void fill_volumes(Session& s, LiveControl& c) {
        std::vector<std::pair<std::string, std::string>> items;
        if (iequals(c.row.type, "VolumeCostList")) {
            std::string missing;
            for (const VolumeCost& v : component_costs(s, "", install_state::kLocal)) {
                const std::string req = format_size_text(v.cost / 512, ui_text, &missing).value_or(std::to_string(v.cost));
                items.emplace_back(v.volume + "\t" + req, v.volume);
            }
        } else if (hooks.list_volumes) {
            for (const std::string& v : hooks.list_volumes(s)) items.emplace_back(v, v);
        }
        ws->set_items(c.window, items, s.property(c.property));
    }

    // -- conditions, subscriptions, refresh ------------------------------------------------

    void apply_conditions(Session& s, LiveDialog& d) {
        for (auto& [control, rows] : d.model.conditions) {
            LiveControl* c = d.find(control);
            if (c == nullptr) continue;
            for (const auto& [action, condition] : rows) {
                if (!eval(s, condition).value_or(false)) continue;
                if (iequals(action, "Hide")) c->visible = false;
                else if (iequals(action, "Show")) c->visible = true;
                else if (iequals(action, "Disable")) c->enabled = false;
                else if (iequals(action, "Enable")) c->enabled = true;
                else if (iequals(action, "Default")) set_default(d, control);
            }
            sync_state(*c);
        }
    }

    void sync_state(LiveControl& c) {
        if (c.window == 0) return;
        ws->set_visible(c.window, c.visible);
        ws->set_enabled(c.window, c.enabled);
        for (u64 ch : c.children) ws->set_enabled(ch, c.enabled);
    }

    void set_default(LiveDialog& d, const std::string& control) {
        LiveControl* c = d.find(control);
        if (c == nullptr || c->window == 0) return;
        d.default_control = control;
        ws->set_default_button(d.window, c->window);
    }

    /// Re-show what depends on properties after one changed: conditions,
    /// formatted texts, and other controls bound to the property.
    void refresh(Session& s, LiveDialog& d, const LiveControl* changed) {
        for (LiveControl& c : d.controls) {
            if (&c == changed || c.window == 0) continue;
            const std::string& t = c.row.type;
            if (iequals(t, "Text") && !is_subscriber(d, c.row.name)) ws->set_text(c.window, wide(control_text(s, c)));
            if (iequals(t, "CheckBox")) ws->set_checked(c.window, !s.property(c.property).empty());
            if (iequals(t, "RadioButtonGroup"))
                for (usize i = 0; i < c.children.size() && i < c.items.size(); ++i)
                    ws->set_checked(c.children[i], c.items[i].value == s.property(c.property));
            if (iequals(t, "Edit") || iequals(t, "PathEdit")) ws->set_text(c.window, wide(s.property(c.property)));
            if (iequals(t, "ListBox") || iequals(t, "ComboBox") || iequals(t, "ListView")) fill_items(s, c);
            if (iequals(t, "DirectoryList") || iequals(t, "DirectoryCombo")) {
                c.property = resolve_property(s, c.row);
                fill_directory(s, c);
            }
        }
        apply_conditions(s, d);
    }

    bool is_subscriber(const LiveDialog& d, const std::string& control) const {
        for (const SubscriptionRow& r : d.model.subscriptions)
            if (r.control == control) return true;
        return false;
    }

    /// Publish @p event to the controls subscribed to it in @p d (EventMapping).
    void publish(Session& s, LiveDialog& d, const std::string& event, const std::string& argument,
                 const Record* progress = nullptr) {
        for (const SubscriptionRow& r : d.model.subscriptions) {
            if (r.event != event) continue;
            LiveControl* c = d.find(r.control);
            if (c == nullptr) continue;
            set_attribute(s, d, *c, r.attribute, argument, progress);
        }
    }

    void set_attribute(Session& s, LiveDialog& d, LiveControl& c, const std::string& attribute, const std::string& value,
                       const Record* progress) {
        if (iequals(attribute, "Text")) {
            if (c.window) ws->set_text(c.window, wide(value));
        } else if (iequals(attribute, "Visible")) {
            c.visible = !value.empty() && value != "0";
            sync_state(c);
        } else if (iequals(attribute, "Enabled") || iequals(attribute, "Enable")) {
            c.enabled = !value.empty() && value != "0";
            sync_state(c);
        } else if (iequals(attribute, "Progress")) {
            if (progress != nullptr) {
                c.range = std::max<i64>(1, field_int(*progress, 2));
                c.position = std::clamp<i64>(field_int(*progress, 1), 0, c.range);
            }
            if (c.window) ws->set_progress(c.window, static_cast<u32>(c.position), static_cast<u32>(c.range));
        } else if (iequals(attribute, "TimeRemaining")) {
            if (c.window) ws->set_text(c.window, wide(value));
        } else if (iequals(attribute, "BillboardName")) {
            show_billboard(s, d, c.row.name, value.empty() ? nullptr : &value);
        } else if (iequals(attribute, "PropertyValue")) {
            set_bound_property(s, d, c, value);
            refresh(s, d, nullptr);
        } else if (iequals(attribute, "IndirectPropertyName") || iequals(attribute, "PropertyName")) {
            c.property = value;
            refresh(s, d, nullptr);
        } else if (iequals(attribute, "Position")) {
            // "x y width height" in installer units.
            i32 v[4] = {0, 0, 0, 0};
            if (std::sscanf(value.c_str(), "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]) == 4 && c.window) {
                const UnitScale sc = scale();
                ws->move(c.window, {sc.px(v[0]), sc.px(v[1]), sc.px(v[2]), sc.px(v[3])});
            }
        } else {
            log_unimplemented("EventMapping", "attribute " + attribute + " of " + c.row.type);
        }
    }

    // -- billboards --------------------------------------------------------------------------

    u32 show_billboard(Session& s, LiveDialog& d, const std::string& host, const std::string* billboard) {
        LiveControl* c = d.find(host);
        if (c == nullptr || !iequals(c->row.type, "Billboard")) return 1627;
        for (u64 w : d.billboard_windows) ws->destroy(w);
        d.billboard_windows.clear();
        d.billboard.clear();
        if (billboard == nullptr || s.package() == nullptr) return 0;
        const std::vector<ControlRow> rows = load_billboard_controls(*s.package(), *billboard);
        if (rows.empty()) return 1627;
        for (const ControlRow& r : rows) {
            LiveControl bc;
            bc.row = r;
            bc.from_billboard = true;
            create_control(s, d, bc, c->row.x, c->row.y);
            if (bc.window) d.billboard_windows.push_back(bc.window);
        }
        d.billboard_host = host;
        d.billboard = *billboard;
        return 0;
    }

    /// Billboard table: the billboards of @p action whose feature is being
    /// installed, in Ordering order; the first is shown on each Billboard
    /// control of the dialog.
    void billboards_for_action(Session& s, LiveDialog& d, const std::string& action) {
        if (s.package() == nullptr) return;
        const Table* t = s.package()->table("Billboard");
        if (t == nullptr) return;
        std::vector<std::pair<i32, std::string>> boards;
        const auto ib = t->column_index("Billboard"), ifeat = t->column_index("Feature_"),
                   iact = t->column_index("Action"), iord = t->column_index("Ordering");
        if (!ib || !iact) return;
        for (const auto& row : t->rows) {
            if (row[*iact] != action) continue;
            if (ifeat) {
                const auto st = feature_action_state(s, row[*ifeat]);
                if (st && *st != install_state::kLocal && *st != install_state::kSource) continue;
            }
            boards.emplace_back(iord ? std::atoi(row[*iord].c_str()) : 0, row[*ib]);
        }
        if (boards.empty()) return;
        std::stable_sort(boards.begin(), boards.end());
        for (LiveControl& c : d.controls)
            if (iequals(c.row.type, "Billboard")) show_billboard(s, d, c.row.name, &boards.front().second);
    }

    // -- creating and destroying dialogs ------------------------------------------------------

    std::unique_ptr<LiveDialog> create_dialog(Session& s, const std::string& name, u64 owner, bool preview) {
        cache(s);
        if (s.package() == nullptr) return nullptr;
        auto model = load_dialog(*s.package(), name);
        if (!model) return nullptr;
        auto d = std::make_unique<LiveDialog>();
        d->model = std::move(*model);
        d->preview = preview;
        d->default_control = d->model.is_error_dialog() ? std::string() : d->model.row.default_control;
        DialogWindowSpec spec = dialog_window_spec(d->model.row, scale());
        spec.title = wide(s.format(d->model.row.title));
        if (spec.title.empty() && !caption.empty()) spec.title = wide(caption);
        d->window = ws->create_dialog(spec, owner);
        if (d->window == 0) return nullptr;
        // Creation (tab) order: Control_First, then its Control_Next chain,
        // then the rest in table order.
        std::vector<const ControlRow*> order;
        std::set<std::string> placed;
        std::string at = d->model.row.first;
        while (!at.empty() && !placed.contains(at)) {
            const ControlRow* r = d->model.control(at);
            if (r == nullptr) break;
            order.push_back(r);
            placed.insert(at);
            at = r->next;
        }
        for (const ControlRow& r : d->model.controls)
            if (!placed.contains(r.name)) order.push_back(&r);
        d->controls.reserve(order.size());
        for (const ControlRow* r : order) {
            d->controls.push_back(LiveControl{});
            d->controls.back().row = *r;
        }
        for (LiveControl& c : d->controls) create_control(s, *d, c);
        if (!d->default_control.empty()) set_default(*d, d->default_control);
        apply_conditions(s, *d);
        if (!cancel_shown)
            if (LiveControl* cc = d->find(d->model.row.cancel)) {
                cc->visible = false;
                sync_state(*cc);
            }
        if (LiveControl* f = d->find(d->model.row.first); f && f->window) ws->set_focus(f->window);
        return d;
    }

    void destroy_dialog(std::unique_ptr<LiveDialog>& d) {
        if (!d) return;
        if (d->window) ws->destroy(d->window);
        mask_parts.clear();
        d.reset();
    }

    // -- ControlEvents ------------------------------------------------------------------------

    static bool is_bracketed(const std::string& e) { return e.size() >= 3 && e.front() == '[' && e.back() == ']'; }

    /// The property a path event's argument names (`Prop` or `[Prop]` for an
    /// indirect property).
    std::string path_property(Session& s, const std::string& arg) {
        if (is_bracketed(arg)) return s.property(arg.substr(1, arg.size() - 2));
        return arg;
    }

    Outcome run_events(Session& s, LiveDialog& d, const std::string& control) {
        auto it = d.model.events.find(control);
        Outcome out;
        if (it == d.model.events.end()) return out;
        const auto chosen = select_control_events(it->second, [&](const std::string& c) { return eval(s, c); });
        for (const ControlEventRow& r : chosen) {
            const std::string raw = r.event;
            const std::string event = s.format(raw);
            // [Prop] events: the property name is the Event cell itself.
            if (is_bracketed(raw)) {
                const std::string prop = raw.substr(1, raw.size() - 2);
                const std::string arg = r.argument == "{}" ? std::string() : s.format(r.argument);
                if (!d.snapshot.contains(prop))
                    d.snapshot[prop] = s.has_property(prop) ? std::optional(s.property(prop)) : std::nullopt;
                s.set_property(prop, arg);
                refresh(s, d, nullptr);
                continue;
            }
            const std::string arg = s.format(r.argument);
            Outcome step = run_event(s, d, event, arg, r);
            if (step.kind == Outcome::Kind::Blocked) return out;  // "blocks further ControlEvents"
            if (step.kind != Outcome::Kind::None) out = step;
        }
        return out;
    }

    Outcome run_event(Session& s, LiveDialog& d, const std::string& event, const std::string& arg, const ControlEventRow& row) {
        Outcome o;
        if (event == "NewDialog") {
            o.kind = Outcome::Kind::NewDialog;
            o.dialog = arg;
        } else if (event == "SpawnDialog" || event == "SelectionBrowse") {
            const Outcome child = run_child(s, d, arg, std::nullopt);
            if (child.kind == Outcome::Kind::Exit || child.kind == Outcome::Kind::Retry || child.kind == Outcome::Kind::Ignore)
                return child;  // Learn: not for a child, but if authored it ends the wizard
            refresh(s, d, nullptr);
        } else if (event == "SpawnWaitDialog") {
            // "triggers the dialog ... if the expression in the Condition
            // column evaluates as FALSE ... removed as soon as TRUE".
            if (!eval(s, row.condition).value_or(true)) {
                (void)run_child(s, d, arg, row.condition);
                refresh(s, d, nullptr);
            }
        } else if (event == "EndDialog") {
            if (arg == "Exit") o.kind = Outcome::Kind::Exit;
            else if (arg == "Retry") o.kind = Outcome::Kind::Retry;
            else if (arg == "Ignore") o.kind = Outcome::Kind::Ignore;
            else if (arg == "Return") o.kind = Outcome::Kind::Return;
            else if (arg.rfind("Error", 0) == 0) {
                static const std::pair<const char*, i32> kButtons[] = {
                    {"ErrorOk", IDOK}, {"ErrorCancel", IDCANCEL}, {"ErrorAbort", IDABORT}, {"ErrorRetry", IDRETRY},
                    {"ErrorIgnore", IDIGNORE}, {"ErrorYes", IDYES}, {"ErrorNo", IDNO}};
                for (const auto& [n, id] : kButtons)
                    if (arg == n) o = {Outcome::Kind::ErrorButton, {}, id};
            } else {
                log_unimplemented("EndDialog", "argument " + arg);
            }
        } else if (event == "DoAction") {
            const u32 rc = do_action(s, arg);
            if (rc == kActionUserExit) o.kind = Outcome::Kind::Exit;
            refresh(s, d, nullptr);
        } else if (event == "AddLocal" || event == "AddSource" || event == "Remove") {
            const i32 st = event == "AddLocal" ? install_state::kLocal
                           : event == "AddSource" ? install_state::kSource : install_state::kAbsent;
            if (arg == "ALL") {
                for (const std::string& f : feature_model(s).feature_order) (void)set_feature_state(s, f, st);
            } else {
                (void)set_feature_state(s, arg, st);
            }
            recompute_states(s);
            refresh(s, d, nullptr);
            for (LiveControl& c : d.controls)
                if (iequals(c.row.type, "SelectionTree")) fill_tree(s, d, c);
        } else if (event == "Reinstall") {
            // REINSTALL takes ALL or a comma-separated feature list.
            std::string v = s.property("REINSTALL");
            if (arg == "ALL" || v == "ALL") v = "ALL";
            else v = v.empty() ? arg : v + "," + arg;
            s.set_property("REINSTALL", v);
        } else if (event == "ReinstallMode") {
            s.set_property("REINSTALLMODE", arg);
        } else if (event == "SetTargetPath" || event == "CheckTargetPath" || event == "CheckExistingTargetPath") {
            const std::string prop = path_property(s, row.argument);
            const std::string path = s.property(prop);
            const bool writable = event != "CheckTargetPath";
            const bool ok = hooks.check_path ? hooks.check_path(s, path, writable) : valid_path_syntax(path);
            if (!ok) return {Outcome::Kind::Blocked, {}, 0};
            if (event == "SetTargetPath" && set_target_path(s, prop, path) != 0) return {Outcome::Kind::Blocked, {}, 0};
            refresh(s, d, nullptr);
        } else if (event == "SetInstallLevel") {
            (void)set_install_level(s, std::atoi(arg.c_str()));
            refresh(s, d, nullptr);
        } else if (event == "Reset") {
            for (const auto& [prop, value] : d.snapshot) s.set_property(prop, value.value_or(std::string()));
            d.snapshot.clear();
            refresh(s, d, nullptr);
        } else if (event == "ValidateProductID") {
            (void)do_action(s, "ValidateProductID");
            refresh(s, d, nullptr);
        } else if (event == "EnableRollback") {
            if (hooks.enable_rollback) hooks.enable_rollback(s, arg == "True");
            else log_unimplemented("EnableRollback", "no engine hook");
        } else if (event == "DirectoryListUp" || event == "DirectoryListOpen" || event == "DirectoryListNew") {
            directory_event(s, d, event);
        } else if (event == "MsiLaunchApp") {
            if (!hooks.launch_app || !hooks.launch_app(s, arg, {})) log_unimplemented("MsiLaunchApp", "launch failed or no hook");
        } else if (event == "RmShutdownAndRestart") {
            rm_shutdown_requested = true;
        } else if (event == "MsiPrint") {
            log_unimplemented("MsiPrint", "printing a ScrollableText control");
        } else {
            // Any other event is published to its subscribers on this dialog
            // (a ProgressBar may subscribe to an event named after an action).
            publish(s, d, event, arg);
        }
        return o;
    }
    bool rm_shutdown_requested{false};

    static bool valid_path_syntax(const std::string& p) {
        // A drive path "X:\..." or a UNC path "\\server\share", with no
        // characters a path may not contain.
        if (p.size() >= 3 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':' && p[2] == '\\') {
        } else if (p.size() >= 5 && p[0] == '\\' && p[1] == '\\') {
        } else {
            return false;
        }
        for (usize i = 2; i < p.size(); ++i) {
            const char c = p[i];
            if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*' || c == ':' ||
                static_cast<unsigned char>(c) < 32)
                return false;
        }
        return true;
    }

    void directory_event(Session& s, LiveDialog& d, const std::string& event) {
        for (LiveControl& c : d.controls) {
            if (!iequals(c.row.type, "DirectoryList")) continue;
            const std::string path = with_slash(s.property(c.property));
            if (event == "DirectoryListUp") {
                s.set_property(c.property, parent_of(path));
            } else if (event == "DirectoryListOpen") {
                if (c.selected.empty()) continue;
                s.set_property(c.property, path + c.selected + "\\");
                c.selected.clear();
            } else {
                // UIText NewFolder is SFN|LFN: the long name. Created once.
                std::string name = ui("NewFolder");
                if (const usize bar = name.find('|'); bar != std::string::npos) name = name.substr(bar + 1);
                if (name.empty()) {
                    log_unimplemented("DirectoryListNew", "UIText NewFolder missing");
                    continue;
                }
                if (c.new_folder.empty()) {
                    if (hooks.create_folder && hooks.create_folder(s, path + name)) c.new_folder = name;
                    else log_unimplemented("DirectoryListNew", "folder creation failed or no hook");
                }
                c.selected = c.new_folder;
            }
        }
        refresh(s, d, nullptr);
    }

    u32 do_action(Session& s, const std::string& name) {
        if (hooks.do_action) return hooks.do_action(s, name);
        if (Engine* e = s.engine()) return e->do_action(name);
        log_unimplemented("DoAction", "no engine to run " + name);
        return kActionFailure;
    }

    // -- user input -----------------------------------------------------------------------------

    Outcome on_input(Session& s, LiveDialog& d, const UiInput& in) {
        if (in.kind == UiInput::Kind::Close) {
            if (d.model.row.cancel.empty() || d.model.is_error_dialog()) return {};
            LiveControl* c = d.find(d.model.row.cancel);
            if (c == nullptr || !c->enabled) return {};
            return run_events(s, d, c->row.name);
        }
        LiveControl* c = d.find_window(in.window);
        if (c == nullptr) return {};
        const std::string& t = c->row.type;
        if (in.kind == UiInput::Kind::Activated) {
            if (!c->enabled) return {};
            if (iequals(t, "CheckBox")) {
                const bool now = s.property(c->property).empty();
                set_bound_property(s, d, *c, now ? c->checkbox_value : std::string());
                ws->set_checked(c->window, now);
                refresh(s, d, c);
            }
            return run_events(s, d, c->row.name);
        }
        if (in.kind == UiInput::Kind::Changed) {
            if (iequals(t, "SelectionTree")) {
                tree_pick(s, d, *c, in.value);
                return run_events(s, d, c->row.name);
            }
            if (iequals(t, "RadioButtonGroup")) {
                for (usize i = 0; i < c->children.size(); ++i)
                    if (c->children[i] == in.window && i < c->items.size()) {
                        set_bound_property(s, d, *c, c->items[i].value);
                        for (usize k = 0; k < c->children.size(); ++k) ws->set_checked(c->children[k], k == i);
                    }
            } else if (iequals(t, "MaskedEdit")) {
                for (usize i = 0; i < c->children.size(); ++i)
                    if (c->children[i] == in.window)
                        set_bound_property(s, d, *c, mask_value(*c, i, in.value, mask_parts[c]));
            } else if (iequals(t, "Edit") || iequals(t, "PathEdit") || iequals(t, "ComboBox") || iequals(t, "ListBox") ||
                       iequals(t, "ListView") || iequals(t, "DirectoryCombo") || iequals(t, "VolumeSelectCombo")) {
                std::string v = in.value;
                if (c->limit && *c->limit > 0 && static_cast<i64>(v.size()) > *c->limit) v.resize(static_cast<usize>(*c->limit));
                set_bound_property(s, d, *c, v);
            }
            refresh(s, d, c);
            return {};
        }
        if (in.kind == UiInput::Kind::Selected) {
            if (iequals(t, "SelectionTree")) {
                c->selected = in.value;
                publish_selection(s, d, *c);
                return run_events(s, d, c->row.name);
            }
            if (iequals(t, "DirectoryList")) {
                c->selected = in.value;
                publish(s, d, "IgnoreChange", in.value);
            }
        }
        return {};
    }

    // -- modal loops ----------------------------------------------------------------------------

    Outcome modal_loop(Session& s, LiveDialog& d) {
        for (;;) {
            if (d.wait_condition && eval(s, *d.wait_condition).value_or(false)) return {Outcome::Kind::Return, {}, 0};
            const UiInput in = ws->wait_input(d.window, d.wait_condition ? 100u : 0xFFFFFFFFu);
            if (in.kind == UiInput::Kind::None) continue;
            const Outcome o = on_input(s, d, in);
            if (o.kind != Outcome::Kind::None && o.kind != Outcome::Kind::Blocked) return o;
        }
    }

    Outcome run_child(Session& s, LiveDialog& parent, const std::string& name, std::optional<std::string> wait) {
        auto child = create_dialog(s, name, parent.window, false);
        if (!child) {
            log_unimplemented("SpawnDialog", "dialog " + name + " missing or could not be created");
            return {};
        }
        child->wait_condition = std::move(wait);
        notify_show_dialog(s, name);
        ws->show_dialog(child->window, child->model.row.hcenter, child->model.row.vcenter, true);
        stack.push_back(std::move(child));
        LiveDialog& ref = *stack.back();
        const Outcome o = modal_loop(s, ref);
        destroy_dialog(stack.back());
        stack.pop_back();
        return o;
    }

    void notify_show_dialog(Session& s, const std::string& name) {
        // INSTALLMESSAGE_SHOWDIALOG: "Sent prior to display of the Full UI
        // dialog", to an external UI only; the string is the dialog name.
        Record r(0);
        r.fields[0].kind = Record::Kind::String;
        r.fields[0].text = wide(name);
        (void)dispatch_external_ui(mgr, kMsgShowDialog, &r, [&s](const Record& rec) { return s.format_record(rec); });
    }

    u32 run_wizard(Session& s, const std::string& first) {
        if (!ws) {
            log_unimplemented("InternalUI", "dialog " + first + ": no window system in this process");
            return kActionFailure;
        }
        cache(s);
        std::string name = first;
        // A new dialog replaces whatever modeless dialog the sequence left up.
        destroy_dialog(modeless);
        for (;;) {
            auto d = create_dialog(s, name, ui_process_state(mgr).owner, false);
            if (!d) {
                log_unimplemented("InternalUI", "dialog " + name + " missing or could not be created");
                return kActionFailure;
            }
            notify_show_dialog(s, name);
            ws->show_dialog(d->window, d->model.row.hcenter, d->model.row.vcenter, true);
            if (!d->model.is_modal()) {
                // A modeless dialog stays up while the sequence continues
                // (Learn: "A modeless dialog box does not require user input
                // for the installation to continue").
                modeless = std::move(d);
                return kActionSuccess;
            }
            stack.push_back(std::move(d));
            const Outcome o = modal_loop(s, *stack.back());
            destroy_dialog(stack.back());
            stack.pop_back();
            switch (o.kind) {
                case Outcome::Kind::NewDialog: name = o.dialog; continue;
                case Outcome::Kind::Return: return kActionSuccess;
                case Outcome::Kind::Exit: return kActionUserExit;
                case Outcome::Kind::Retry: return kActionSuspend;
                case Outcome::Kind::Ignore: return kActionSkipRemaining;  // "the Finished value"
                default: return kActionSuccess;
            }
        }
    }

    // -- messages ---------------------------------------------------------------------------------

    std::vector<LiveDialog*> visible_dialogs() {
        std::vector<LiveDialog*> out;
        if (modeless) out.push_back(modeless.get());
        for (auto& d : stack) out.push_back(d.get());
        return out;
    }

    void publish_all(Session& s, const std::string& event, const std::string& arg, const Record* rec = nullptr) {
        for (LiveDialog* d : visible_dialogs()) publish(s, *d, event, arg, rec);
    }

    i32 poll_cancel() {
        if (!ws) return 0;
        ws->pump();
        if (basic && basic->cancel != 0) {
            const UiInput in = ws->wait_input(basic->dialog, 0);
            if ((in.kind == UiInput::Kind::Activated && in.window == basic->cancel) || in.kind == UiInput::Kind::Close)
                cancel_pending = true;
        }
        if (modeless) {
            const UiInput in = ws->wait_input(modeless->window, 0);
            if (in.kind != UiInput::Kind::None) {
                const Outcome o = on_input(*current_session, *modeless, in);
                if (o.kind == Outcome::Kind::Exit) cancel_pending = true;
                if (o.kind == Outcome::Kind::NewDialog) {
                    // A modeless dialog's NewDialog / SpawnDialog (e.g. a
                    // cancel confirmation) runs a modal dialog over it.
                    const Outcome child = run_child(*current_session, *modeless, o.dialog, std::nullopt);
                    if (child.kind == Outcome::Kind::Exit) cancel_pending = true;
                }
            }
        }
        if (cancel_pending) {
            cancel_pending = false;
            return IDCANCEL;
        }
        return 0;
    }
    Session* current_session{nullptr};

    void ensure_basic(Session& s) {
        if (basic || !ws || base_level() != sdk::INSTALLUILEVEL_BASIC) return;
        // Vitreo's own basic-UI progress window (Windows draws a built-in one).
        DialogWindowSpec spec;
        spec.class_name = u"MsiDialogNoCloseClass";
        spec.no_close = true;
        spec.style = 0x04C80000;  // WS_CLIPSIBLINGS | WS_CAPTION | WS_SYSMENU
        spec.ex_style = 0x00040100;
        spec.client_w = 380;
        spec.client_h = 112;
        spec.title = wide(caption.empty() ? s.property("ProductName") : caption);
        Basic b;
        b.dialog = ws->create_dialog(spec, ui_process_state(mgr).owner);
        if (b.dialog == 0) return;
        WindowSpec text;
        text.class_name = u"Static";
        text.style = WS_CHILD | WS_VISIBLE | WS_GROUP | 0x80;  // SS_NOPREFIX
        text.rect = {16, 16, 348, 16};
        b.text = ws->create_control(b.dialog, text);
        WindowSpec bar;
        bar.class_name = u"msctls_progress32";
        bar.style = WS_CHILD | WS_VISIBLE | WS_GROUP;
        bar.rect = {16, 42, 348, 16};
        b.bar = ws->create_control(b.dialog, bar);
        const bool hide_cancel = (level & sdk::INSTALLUILEVEL_HIDECANCEL) != 0 || !cancel_shown;
        if (!hide_cancel) {
            WindowSpec cancel;
            cancel.class_name = u"Button";
            cancel.style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP;
            cancel.rect = {284, 74, 80, 24};
            cancel.text = u"Cancel";
            b.cancel = ws->create_control(b.dialog, cancel);
        }
        ws->show_dialog(b.dialog, 50, 50, false);
        basic = b;
    }

    void end_basic() {
        if (basic && ws) ws->destroy(basic->dialog);
        basic.reset();
    }

    void publish_progress(Session& s) {
        const i64 total = std::max<i64>(progress.total, 1);
        const i64 pos = std::clamp<i64>(progress.position, 0, total);
        Record r(3);
        r.fields[1].kind = Record::Kind::Integer;
        r.fields[1].integer = static_cast<i32>(std::min<i64>(pos, 0x7FFFFFFF));
        r.fields[2].kind = Record::Kind::Integer;
        r.fields[2].integer = static_cast<i32>(std::min<i64>(total, 0x7FFFFFFF));
        r.fields[3].kind = Record::Kind::String;
        r.fields[3].text = wide(progress.action);
        publish_all(s, "SetProgress", {}, &r);
        if (basic) ws->set_progress(basic->bar, static_cast<u32>(pos * 1024 / total), 1024);
        // TimeRemaining: UIText TimeRemaining formatted with [1] minutes and
        // [2] seconds; a zero part is null so its {..} block drops out.
        if (!progress.script && pos > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - progress.reset_at).count();
            const i64 remaining = elapsed * (total - pos) / pos;
            Record t(2);
            t.fields[0].kind = Record::Kind::String;
            t.fields[0].text = wide(ui("TimeRemaining"));
            if (remaining / 60 > 0) t.fields[1] = {Record::Kind::Integer, static_cast<i32>(remaining / 60), {}, {}, 0};
            if (remaining % 60 > 0) t.fields[2] = {Record::Kind::Integer, static_cast<i32>(remaining % 60), {}, {}, 0};
            publish_all(s, "TimeRemaining", s.format_record(t));
        }
    }

    i32 on_progress(Session& s, const Record& r) {
        switch (field_int(r, 1)) {
            case 0:  // Reset
                progress.started = true;
                progress.total = field_int(r, 2);
                progress.forward = field_int(r, 3) == 0;
                progress.position = progress.forward ? 0 : progress.total;
                progress.script = field_int(r, 4) == 1;
                progress.reset_at = std::chrono::steady_clock::now();
                publish_all(s, "ScriptInProgress", progress.script ? ui("ScriptInProgress") : std::string());
                break;
            case 1:  // ActionInfo
                progress.per_action_data = field_int(r, 3) == 1 ? field_int(r, 2) : 0;
                break;
            case 2:  // ProgressReport
                if (!progress.started) return 0;  // "should not act ... until the first Reset"
                progress.position += progress.forward ? field_int(r, 2) : -field_int(r, 2);
                break;
            case 3:  // ProgressAddition
                if (!progress.started) return 0;
                progress.total += field_int(r, 2);
                break;
            default: return 0;
        }
        ensure_basic(s);
        if (progress.started) publish_progress(s);
        return poll_cancel() == IDCANCEL ? IDCANCEL : 0;
    }

    i32 on_action_start(Session& s, const Record& r) {
        // [1] action name, [2] description, [3] ActionData template.
        progress.action = field_text(r, 1);
        progress.per_action_data = 0;
        const std::string description = field_text(r, 2);
        publish_all(s, "ActionText", description.empty() ? progress.action : description);
        for (LiveDialog* d : visible_dialogs()) {
            for (const SubscriptionRow& sub : d->model.subscriptions)
                if (sub.event == "ActionData")
                    if (LiveControl* c = d->find(sub.control)) {
                        c->visible = false;  // "hidden when a new action starts"
                        sync_state(*c);
                    }
            billboards_for_action(s, *d, progress.action);
        }
        ensure_basic(s);
        if (basic) ws->set_text(basic->text, wide(description.empty() ? progress.action : description));
        return poll_cancel() == IDCANCEL ? IDCANCEL : 0;
    }

    i32 on_action_data(Session& s, const Record& r) {
        const std::string text = s.format_record(r);
        for (LiveDialog* d : visible_dialogs()) {
            for (const SubscriptionRow& sub : d->model.subscriptions)
                if (sub.event == "ActionData")
                    if (LiveControl* c = d->find(sub.control)) {
                        c->visible = true;  // "shown when new action data arrives"
                        sync_state(*c);
                        set_attribute(s, *d, *c, sub.attribute, text, nullptr);
                    }
        }
        if (progress.per_action_data != 0 && progress.started) {
            progress.position += progress.forward ? progress.per_action_data : -progress.per_action_data;
            publish_progress(s);
        }
        return poll_cancel() == IDCANCEL ? IDCANCEL : 0;
    }

    void on_common_data(Session& s, const Record& r) {
        switch (field_int(r, 1)) {
            case 1: caption = field_text(r, 2); break;  // Caption
            case 2:                                     // CancelShow
                cancel_shown = field_int(r, 2) != 0;
                for (LiveDialog* d : visible_dialogs())
                    if (LiveControl* c = d->find(d->model.row.cancel)) {
                        c->visible = cancel_shown;
                        sync_state(*c);
                    }
                if (basic && basic->cancel) ws->set_visible(basic->cancel, cancel_shown);
                break;
            default: break;  // Language: fonts are created from the package's styles
        }
        (void)s;
    }

    static std::vector<std::pair<const char*, i32>> buttons_for(u32 type) {
        switch (type & MB_TYPEMASK) {
            case MB_OKCANCEL: return {{"ErrorOk", IDOK}, {"ErrorCancel", IDCANCEL}};
            case MB_ABORTRETRYIGNORE: return {{"ErrorAbort", IDABORT}, {"ErrorRetry", IDRETRY}, {"ErrorIgnore", IDIGNORE}};
            case MB_YESNOCANCEL: return {{"ErrorYes", IDYES}, {"ErrorNo", IDNO}, {"ErrorCancel", IDCANCEL}};
            case MB_YESNO: return {{"ErrorYes", IDYES}, {"ErrorNo", IDNO}};
            case MB_RETRYCANCEL: return {{"ErrorRetry", IDRETRY}, {"ErrorCancel", IDCANCEL}};
            default: return {{"ErrorOk", IDOK}};
        }
    }

    /// The authored error dialog (ErrorDialog property). Returns 0 when the
    /// package has none ("the error messages will not be displayed").
    i32 error_dialog(Session& s, u32 type, const std::string& text) {
        const std::string name = s.property("ErrorDialog");
        if (name.empty() || !ws) return 0;
        auto d = create_dialog(s, name, stack.empty() ? ui_process_state(mgr).owner : stack.back()->window, false);
        if (!d || !d->model.is_error_dialog()) {
            if (d) destroy_dialog(d);
            return 0;
        }
        if (LiveControl* t = d->find("ErrorText"); t && t->window) ws->set_text(t->window, wide(text));
        if (LiveControl* icon = d->find("ErrorIcon"); icon && icon->window) {
            const u32 kind = type & 0xFF000000u;
            const u32 idi = kind == kMsgFatalExit ? IDI_ERROR : kind == kMsgOutOfDiskSpace ? IDI_INFORMATION : IDI_WARNING;
            ws->set_system_icon(icon->window, idi);
        }
        // Only the message's buttons exist; their X positions are spread
        // evenly over the span the authored buttons occupy (Learn: "The X
        // coordinate of the buttons is modified so the buttons that are
        // displayed are evenly spaced"; the spacing is Vitreo's own).
        const auto wanted = buttons_for(type);
        std::vector<LiveControl*> shown;
        i32 left = 1 << 30, right = 0;
        static const char* kAll[] = {"ErrorOk", "ErrorCancel", "ErrorAbort", "ErrorRetry", "ErrorIgnore", "ErrorYes", "ErrorNo"};
        for (const char* n : kAll) {
            for (LiveControl& c : d->controls) {
                auto it = d->model.events.find(c.row.name);
                if (it == d->model.events.end()) continue;
                bool is_button = false;
                for (const ControlEventRow& e : it->second)
                    if (e.event == "EndDialog" && e.argument == n) is_button = true;
                if (!is_button) continue;
                left = std::min(left, c.row.x);
                right = std::max(right, c.row.x + c.row.width);
                const bool keep = std::any_of(wanted.begin(), wanted.end(), [&](const auto& w) { return std::string_view(w.first) == n; });
                if (keep) shown.push_back(&c);
                else if (c.window) ws->set_visible(c.window, false), c.visible = false;
            }
        }
        if (!shown.empty() && right > left) {
            const UnitScale sc = scale();
            const i32 span = right - left;
            const i32 slot = span / static_cast<i32>(shown.size());
            for (usize i = 0; i < shown.size(); ++i) {
                LiveControl* c = shown[i];
                const i32 x = left + static_cast<i32>(i) * slot + (slot - c->row.width) / 2;
                if (c->window) ws->move(c->window, {sc.px(x), sc.px(c->row.y), sc.px(c->row.width), sc.px(c->row.height)});
            }
            const usize def = ((type & MB_DEFMASK) >> 8);
            if (def < shown.size() && shown[def]->window) ws->set_default_button(d->window, shown[def]->window);
        }
        ws->show_dialog(d->window, d->model.row.hcenter, d->model.row.vcenter, true);
        stack.push_back(std::move(d));
        Outcome o;
        do {
            o = modal_loop(s, *stack.back());
        } while (o.kind != Outcome::Kind::ErrorButton && o.kind != Outcome::Kind::Exit);
        destroy_dialog(stack.back());
        stack.pop_back();
        return o.kind == Outcome::Kind::ErrorButton ? o.button : IDCANCEL;
    }

    i32 files_in_use(Session& s, const Record& r, bool restart_manager) {
        const std::string name = restart_manager ? "MsiRMFilesInUse" : "FilesInUse";
        if (s.package() == nullptr || !load_dialog(*s.package(), name)) return 0;
        // The record's fields are (process name, window title) pairs; the
        // dialog's ListBox lists them through FileInUseProcess.
        auto d = create_dialog(s, name, stack.empty() ? ui_process_state(mgr).owner : stack.back()->window, false);
        if (!d) return 0;
        for (LiveControl& c : d->controls) {
            if (!iequals(c.row.type, "ListBox") || c.property != "FileInUseProcess") continue;
            std::vector<std::pair<std::string, std::string>> items;
            for (u32 i = 1; i + 1 <= r.count(); i += 2) {
                const std::string proc = field_text(r, i), title = field_text(r, i + 1);
                items.emplace_back(title.empty() ? proc : title, proc);
            }
            ws->set_items(c.window, items, {});
        }
        ws->show_dialog(d->window, d->model.row.hcenter, d->model.row.vcenter, true);
        stack.push_back(std::move(d));
        rm_shutdown_requested = false;
        const Outcome o = modal_loop(s, *stack.back());
        destroy_dialog(stack.back());
        stack.pop_back();
        switch (o.kind) {
            case Outcome::Kind::Retry: return IDRETRY;
            case Outcome::Kind::Ignore: return IDIGNORE;
            case Outcome::Kind::Exit: return IDCANCEL;
            case Outcome::Kind::Return: return rm_shutdown_requested ? IDOK : IDIGNORE;
            default: return 0;
        }
    }

    i32 on_error(Session& s, u32 type, const Record& r) {
        const u32 base = base_level();
        if (base == sdk::INSTALLUILEVEL_NONE) return 0;
        const std::string text = s.format_record(r);
        if (base == sdk::INSTALLUILEVEL_FULL || base == sdk::INSTALLUILEVEL_REDUCED || base == sdk::INSTALLUILEVEL_DEFAULT)
            return error_dialog(s, type, text);
        // Basic UI: "Displays built-in error dialog boxes" — a message box;
        // PROGRESSONLY shows none.
        if (level & sdk::INSTALLUILEVEL_PROGRESSONLY) return 0;
        if (!ws) return 0;
        const std::string title = caption.empty() ? s.property("ProductName") : caption;
        return ws->message_box(basic ? basic->dialog : ui_process_state(mgr).owner, wide(text), wide(title),
                               type & (MB_TYPEMASK | MB_ICONMASK | MB_DEFMASK));
    }

    void on_install_end(Session& s, const Record& r) {
        // INSTALLUILEVEL_ENDDIALOG: a modal box at the end of a successful
        // install or after an error, none after a user cancel. Wording is
        // Vitreo's own.
        if (!(level & sdk::INSTALLUILEVEL_ENDDIALOG) || !ws) return;
        const i64 rc = field_int(r, 3);
        if (rc == kActionUserExit) return;
        const std::string product = field_text(r, 1).empty() ? s.property("ProductName") : field_text(r, 1);
        const std::string text = (rc == 0 || rc == 3010 || rc == 1641)
                                     ? product + " has been installed."
                                     : product + " could not be installed (error " + std::to_string(rc) + ").";
        (void)ws->message_box(ui_process_state(mgr).owner, wide(text), wide(product), rc == 0 ? 0x40u : 0x10u);
    }

    i32 message(Session& s, u32 type, const Record& r) {
        current_session = &s;
        cache(s);
        const u32 kind = type & 0xFF000000u;
        const u32 base = base_level();
        if (base == sdk::INSTALLUILEVEL_NONE && kind != kMsgResolveSource) return 0;
        switch (kind) {
            case kMsgFatalExit:
            case kMsgError:
            case kMsgWarning:
            case kMsgUser:
            case kMsgOutOfDiskSpace: return on_error(s, type, r);
            case kMsgInfo: return 0;  // log only
            case kMsgFilesInUse:
                if (base == sdk::INSTALLUILEVEL_BASIC) return 0;
                return files_in_use(s, r, false);
            case kMsgRmFilesInUse:
                if (base != sdk::INSTALLUILEVEL_FULL && base != sdk::INSTALLUILEVEL_DEFAULT) return 0;
                return files_in_use(s, r, true);
            case kMsgResolveSource: return 0;  // the engine resolves the source
            case kMsgActionStart: return on_action_start(s, r);
            case kMsgActionData: return on_action_data(s, r);
            case kMsgProgress: return on_progress(s, r);
            case kMsgCommonData: on_common_data(s, r); return 0;
            case kMsgInitialize: return 0;
            case kMsgTerminate:
                end_basic();
                destroy_dialog(modeless);
                return 0;
            case kMsgInstallEnd: on_install_end(s, r); return 0;
            default: return 0;
        }
    }
};

InstallerUi::InstallerUi(MsiManager& mgr, std::shared_ptr<WindowSystem> windows, UiHooks hooks, u32 level)
    : m_impl(std::make_unique<Impl>(mgr, std::move(windows), std::move(hooks), level)), m_mgr(mgr), m_fixed_level(level) {}

u32 InstallerUi::level_for(Session* session) const {
    if (m_fixed_level != 0) return m_fixed_level;
    if (session != nullptr && session->engine() != nullptr) return session->engine()->ui_level();
    return m_mgr.engine_ui_level();
}

u32 InstallerUi::level() const { return level_for(m_impl->current_session); }

InstallerUi::~InstallerUi() {
    if (m_impl) {
        m_impl->end_basic();
        m_impl->destroy_dialog(m_impl->modeless);
        while (!m_impl->stack.empty()) {
            m_impl->destroy_dialog(m_impl->stack.back());
            m_impl->stack.pop_back();
        }
    }
}

u32 InstallerUi::run_dialog(Session& session, const std::string& dialog) {
    m_impl->current_session = &session;
    m_impl->level = level_for(&session);
    const u32 base = m_impl->level & ~kUiLevelModifiers;
    if (base == sdk::INSTALLUILEVEL_NONE || base == sdk::INSTALLUILEVEL_BASIC) return kActionNotCalled;
    if (session.package() == nullptr || !load_dialog(*session.package(), dialog)) return kActionNotCalled;
    if (base == sdk::INSTALLUILEVEL_REDUCED) {
        // Reduced UI shows the modeless dialogs only.
        auto model = load_dialog(*session.package(), dialog);
        if (model->is_modal()) return kActionNotCalled;
    }
    m_impl->current_session = &session;
    return m_impl->run_wizard(session, dialog);
}

i32 InstallerUi::message(Session& session, u32 type, const Record& record) {
    m_impl->level = level_for(&session);
    return m_impl->message(session, type, record);
}

std::string InstallerUi::current_dialog() const {
    if (!m_impl->stack.empty()) return m_impl->stack.back()->model.row.name;
    if (m_impl->modeless) return m_impl->modeless->model.row.name;
    return {};
}

// ---- UI preview (msi_ui.cpp exports) -----------------------------------------------------

u32 preview_show_dialog(InstallerUi& ui, Session& session, const std::string& dialog);
u32 preview_show_billboard(InstallerUi& ui, Session& session, const std::string& control, const std::string* billboard);

struct InstallerUiPreviewAccess {
    static u32 show(InstallerUi& ui, Session& s, const std::string& name) {
        auto& impl = *ui.m_impl;
        impl.destroy_dialog(impl.modeless);
        if (name.empty()) return 0;
        // "Display any UI dialog as modeless and inactive."
        auto d = impl.create_dialog(s, name, 0, true);
        if (!d) return 1626;
        impl.ws->show_dialog(d->window, d->model.row.hcenter, d->model.row.vcenter, false);
        impl.modeless = std::move(d);
        return 0;
    }
    static u32 billboard(InstallerUi& ui, Session& s, const std::string& control, const std::string* bb) {
        auto& impl = *ui.m_impl;
        if (!impl.modeless) return bb == nullptr ? 0u : 1627u;
        return impl.show_billboard(s, *impl.modeless, control, bb);
    }
};

u32 preview_show_dialog(InstallerUi& ui, Session& session, const std::string& dialog) {
    return InstallerUiPreviewAccess::show(ui, session, dialog);
}
u32 preview_show_billboard(InstallerUi& ui, Session& session, const std::string& control, const std::string* billboard) {
    return InstallerUiPreviewAccess::billboard(ui, session, control, billboard);
}

}  // namespace vitreo::winapi::msi
