// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_msi_ui.cpp
/// @brief Contract tests for the Windows Installer ui area, both bitnesses
/// through msi_fixture.hpp.
///
/// Expected values come from Windows: the msi_ui_api*.expected and
/// msi_ui_layout*.expected outputs of tools/oracle_probe/msi_ui_*_probe.c
/// (read at run time, not transcribed), the SDK headers, and Learn. The
/// dialog engine runs against a recording WindowSystem.

#include "msi_fixture.hpp"

#include "capsule/vfs/host_filesystem.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/system/msi/msi_engine.hpp"
#include "winapi/system/msi/msi_package.hpp"
#include "winapi/system/msi/msi_ui.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"

#include <deque>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <unistd.h>

namespace vitreo::winapi::msi_test {
namespace {

namespace fs = std::filesystem;
using namespace vitreo::winapi::msi;

const fs::path kRepo = fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path();

std::vector<std::string> expected_lines(const std::string& file) {
    std::ifstream in(kRepo / "tests" / "oracle_data" / file);
    std::vector<std::string> out;
    for (std::string line; std::getline(in, line);) out.push_back(line);
    return out;
}

std::string narrow(const std::u16string& s) {
    std::vector<u16> w(s.begin(), s.end());
    return utf16_to_utf8(w.data(), static_cast<int>(w.size()));
}

// ---- A package built in memory -------------------------------------------------------

struct TableDef {
    std::string name;
    std::vector<std::string> columns;
    usize keys;
    std::vector<std::vector<std::string>> rows;
};

std::shared_ptr<Package> make_package(const std::vector<TableDef>& tables) {
    auto pkg = std::make_shared<Package>(Package::create());
    for (const TableDef& t : tables) {
        std::vector<ColumnDef> cols;
        for (usize i = 0; i < t.columns.size(); ++i)
            cols.push_back(make_column(t.columns[i], 's', 255, i >= t.keys, i < t.keys, false, false));
        EXPECT_EQ(pkg->create_table(t.name, cols, false), 0u) << t.name;
        Table* table = pkg->find_table(t.name);
        for (const auto& r : t.rows) {
            std::vector<std::string> cells = r;
            cells.resize(t.columns.size());
            EXPECT_EQ(pkg->insert_row(*table, cells, false), 0u) << t.name;
        }
    }
    return pkg;
}

const std::vector<std::string> kDialogCols = {"Dialog", "HCentering", "VCentering", "Width", "Height", "Attributes",
                                              "Title", "Control_First", "Control_Default", "Control_Cancel"};
const std::vector<std::string> kControlCols = {"Dialog_", "Control", "Type", "X", "Y", "Width", "Height",
                                               "Attributes", "Property", "Text", "Control_Next", "Help"};
const std::vector<std::string> kEventCols = {"Dialog_", "Control_", "Event", "Argument", "Condition", "Ordering"};

// ---- A recording WindowSystem ------------------------------------------------------------

class FakeWindows : public WindowSystem {
public:
    struct Win {
        u64 parent{0};
        bool dialog{false};
        std::string cls, text;
        u32 style{0}, ex{0};
        PixelRect rect;
        bool visible{true}, enabled{true}, checked{false};
        std::vector<std::pair<std::string, std::string>> items;
        u32 pos{0}, range{0};
        std::optional<FontSpec> font;
        bool destroyed{false};
        u32 icon{0};
    };
    std::map<u64, Win> wins;
    u64 next{100};
    std::deque<std::function<UiInput(FakeWindows&, u64)>> script;
    std::vector<std::string> dialogs_shown;
    std::vector<std::tuple<std::string, std::string, u32>> boxes;
    i32 box_answer{1};

    i32 dpi() const override { return 96; }
    i32 system_text_height() const override { return 16; }
    std::pair<i32, i32> text_extent(const FontSpec& f, const std::u16string& t) override {
        // Tahoma 8 metrics at 96 DPI (digits 6 px, '-' 4 px, height 13).
        (void)f;
        i32 w = 0;
        for (char16_t c : t) w += (c == u'-') ? 4 : 6;
        return {w, 13};
    }
    u64 create_dialog(const DialogWindowSpec& s, u64 owner) override {
        const u64 h = next++;
        Win w;
        w.parent = owner;
        w.dialog = true;
        w.cls = narrow(s.class_name);
        w.text = narrow(s.title);
        w.style = s.style;
        w.ex = s.ex_style;
        w.rect = {0, 0, s.client_w, s.client_h};
        w.visible = false;
        wins[h] = w;
        return h;
    }
    u64 create_control(u64 parent, const WindowSpec& s) override {
        const u64 h = next++;
        Win w;
        w.parent = parent;
        w.cls = narrow(s.class_name);
        w.text = narrow(s.text);
        w.style = s.style;
        w.ex = s.ex_style;
        w.rect = s.rect;
        w.visible = (s.style & 0x10000000) != 0;
        w.enabled = (s.style & 0x08000000) == 0;
        wins[h] = w;
        return h;
    }
    void set_font(u64 w, const FontSpec& f, std::optional<u32>) override { wins[w].font = f; }
    void set_text(u64 w, const std::u16string& t) override { wins[w].text = narrow(t); }
    void set_rich_text(u64 w, const std::string& rtf) override { wins[w].text = rtf; }
    void set_visible(u64 w, bool v) override { wins[w].visible = v; }
    void set_enabled(u64 w, bool e) override { wins[w].enabled = e; }
    void set_checked(u64 w, bool c) override { wins[w].checked = c; }
    void set_default_button(u64, u64) override {}
    void set_focus(u64) override {}
    void set_image(u64, const std::vector<u8>&, bool, i32) override {}
    void set_system_icon(u64 w, u32 idi) override { wins[w].icon = idi; }
    void set_items(u64 w, const std::vector<std::pair<std::string, std::string>>& items, const std::string&) override {
        wins[w].items = items;
    }
    void set_progress(u64 w, u32 p, u32 r) override {
        wins[w].pos = p;
        wins[w].range = r;
    }
    void set_tree(u64, const std::vector<TreeNode>&, const std::string&) override {}
    void move(u64 w, const PixelRect& r) override { wins[w].rect = r; }
    void show_dialog(u64 d, i32, i32, bool) override {
        wins[d].visible = true;
        dialogs_shown.push_back(wins[d].text);
    }
    void destroy(u64 w) override {
        wins[w].destroyed = true;
        for (auto& [h, x] : wins)
            if (x.parent == w) destroy(h);
    }
    UiInput wait_input(u64 dialog, u32 timeout) override {
        if (script.empty()) {
            if (timeout != 0xFFFFFFFFu) return {};
            ADD_FAILURE() << "dialog loop waited with no scripted input";
            return {UiInput::Kind::Close, 0, {}};
        }
        auto step = script.front();
        script.pop_front();
        return step(*this, dialog);
    }
    void pump() override {}
    i32 message_box(u64, const std::u16string& text, const std::u16string& caption, u32 type) override {
        boxes.emplace_back(narrow(text), narrow(caption), type);
        return box_answer;
    }

    /// The live window of @p dialog (or any descendant) whose text is @p text.
    u64 by_text(u64 dialog, const std::string& text) {
        for (auto& [h, w] : wins)
            if (!w.destroyed && w.text == text && descends(h, dialog)) return h;
        ADD_FAILURE() << "no window '" << text << "'";
        return 0;
    }
    bool descends(u64 h, u64 root) {
        for (u64 p = wins[h].parent; p != 0; p = wins[p].parent)
            if (p == root) return true;
        return false;
    }
    void click(const std::string& text) {
        script.push_back([text](FakeWindows& f, u64 d) { return UiInput{UiInput::Kind::Activated, f.by_text(d, text), {}}; });
    }
    void change(const std::string& text, std::string value) {
        script.push_back([text, value](FakeWindows& f, u64 d) {
            return UiInput{UiInput::Kind::Changed, f.by_text(d, text), value};
        });
    }
    u64 live_dialog() {
        u64 last = 0;
        for (auto& [h, w] : wins)
            if (w.dialog && !w.destroyed) last = h;
        return last;
    }
};

// ---- A small wizard package -----------------------------------------------------------------

std::shared_ptr<Package> wizard_package() {
    return make_package({
        {"Property", {"Property", "Value"}, 1,
         {{"ProductName", "Probe"}, {"DefaultUIFont", "Dflt"}, {"ErrorDialog", "ErrDlg"}, {"CHOICE", "b"}}},
        {"TextStyle", {"TextStyle", "FaceName", "Size", "Color", "StyleBits"}, 1,
         {{"Dflt", "Tahoma", "8", "", "0"}, {"Big", "Tahoma", "12", "255", "5"}}},
        {"Dialog", kDialogCols, 1,
         {{"Welcome", "50", "50", "370", "270", "3", "[ProductName] Setup", "Next", "Next", "Cancel"},
          {"Second", "50", "50", "370", "270", "3", "Second", "Finish", "Finish", ""},
          {"CancelDlg", "50", "50", "200", "80", "3", "Sure?", "No", "No", "No"},
          {"ErrDlg", "50", "50", "270", "105", "65539", "Error", "ErrorText", "", ""},
          {"ProgressDlg", "50", "50", "370", "270", "1", "Installing", "Bar", "", "Cancel"}}},
        {"Control", kControlCols, 2,
         {{"Welcome", "Next", "PushButton", "236", "243", "56", "17", "3", "", "&Next", "Cancel", ""},
          {"Welcome", "Cancel", "PushButton", "304", "243", "56", "17", "3", "", "Cancel", "Check", ""},
          {"Welcome", "Title", "Text", "20", "15", "220", "60", "196611", "", "{\\Big}Hello [ProductName]", "", ""},
          {"Welcome", "Check", "CheckBox", "20", "100", "200", "12", "3", "CHK", "I agree", "Edit", ""},
          {"Welcome", "Edit", "Edit", "20", "120", "200", "18", "7", "EDITP", "{5}", "Radio", ""},
          {"Welcome", "Radio", "RadioButtonGroup", "20", "150", "200", "40", "3", "CHOICE", "", "Next", ""},
          {"Welcome", "Echo", "Text", "20", "200", "200", "12", "3", "", "Edit=[EDITP]", "", ""},
          {"Second", "Finish", "PushButton", "236", "243", "56", "17", "3", "", "Finish", "", ""},
          {"CancelDlg", "Yes", "PushButton", "20", "50", "56", "17", "3", "", "Yes", "No", ""},
          {"CancelDlg", "No", "PushButton", "100", "50", "56", "17", "3", "", "No", "Yes", ""},
          {"ErrDlg", "ErrorText", "Text", "48", "15", "205", "60", "131075", "", "", "", ""},
          {"ErrDlg", "ErrorIcon", "Icon", "15", "15", "24", "24", "5242881", "", "", "", ""},
          {"ErrDlg", "Y", "PushButton", "100", "80", "56", "17", "3", "", "Yes", "", ""},
          {"ErrDlg", "N", "PushButton", "100", "80", "56", "17", "3", "", "No", "", ""},
          {"ErrDlg", "O", "PushButton", "100", "80", "56", "17", "3", "", "OK", "", ""},
          {"ErrDlg", "C", "PushButton", "100", "80", "56", "17", "3", "", "Cancel", "", ""},
          {"ProgressDlg", "Bar", "ProgressBar", "20", "100", "330", "10", "1", "", "", "", ""},
          {"ProgressDlg", "Act", "Text", "20", "80", "330", "12", "3", "", "", "", ""},
          {"ProgressDlg", "Data", "Text", "20", "120", "330", "12", "3", "", "", "", ""},
          {"ProgressDlg", "Cancel", "PushButton", "304", "243", "56", "17", "3", "", "Cancel", "", ""}}},
        {"ControlEvent", kEventCols, 5,
         {{"Welcome", "Next", "[NEXTCLICKED]", "yes[ProductName]", "1", "1"},
          {"Welcome", "Next", "NewDialog", "Second", "1", "2"},
          {"Welcome", "Next", "NewDialog", "Other", "0", "3"},
          {"Welcome", "Cancel", "SpawnDialog", "CancelDlg", "1", "1"},
          {"CancelDlg", "Yes", "EndDialog", "Exit", "1", "1"},
          {"CancelDlg", "No", "EndDialog", "Return", "1", "1"},
          {"Second", "Finish", "EndDialog", "Return", "1", "1"},
          {"ErrDlg", "Y", "EndDialog", "ErrorYes", "1", "1"},
          {"ErrDlg", "N", "EndDialog", "ErrorNo", "1", "1"},
          {"ErrDlg", "O", "EndDialog", "ErrorOk", "1", "1"},
          {"ErrDlg", "C", "EndDialog", "ErrorCancel", "1", "1"}}},
        {"ControlCondition", {"Dialog_", "Control_", "Action", "Condition"}, 4,
         {{"Welcome", "Next", "Disable", "NOT CHK"}, {"Welcome", "Next", "Enable", "CHK"}}},
        {"EventMapping", {"Dialog_", "Control_", "Event", "Attribute"}, 3,
         {{"ProgressDlg", "Bar", "SetProgress", "Progress"},
          {"ProgressDlg", "Act", "ActionText", "Text"},
          {"ProgressDlg", "Data", "ActionData", "Text"}}},
        {"CheckBox", {"Property", "Value"}, 1, {{"CHK", "agreed"}}},
        {"RadioButton", {"Property", "Order", "Value", "X", "Y", "Width", "Height", "Text", "Help"}, 2,
         {{"CHOICE", "1", "a", "5", "5", "100", "12", "&Alpha", ""}, {"CHOICE", "2", "b", "5", "20", "100", "12", "{\\Big}Beta", ""}}},
    });
}

class MsiUiEngine : public ::testing::Test {
protected:
    void SetUp() override {
        pkg = wizard_package();
        ASSERT_TRUE(session.initialize(pkg));
        windows = std::make_shared<FakeWindows>();
    }
    InstallerUi make_ui(u32 level = sdk::INSTALLUILEVEL_FULL) {
        UiHooks hooks;
        hooks.do_action = [this](Session&, const std::string& a) { actions.push_back(a); return 0u; };
        return InstallerUi(mgr, windows, hooks, level);
    }
    void TearDown() override { reset_ui_process_state(mgr); }
    MsiManager mgr;
    std::shared_ptr<Package> pkg;
    Session session;
    std::shared_ptr<FakeWindows> windows;
    std::vector<std::string> actions;
};

TEST_F(MsiUiEngine, WizardNextRunsEventsInOrderAndOnlyTheTrueNewDialog) {
    InstallerUi ui = make_ui();
    windows->click("I agree");  // enables Next (ControlCondition)
    windows->click("&Next");
    windows->click("Finish");
    EXPECT_EQ(ui.run_dialog(session, "Welcome"), kActionSuccess);
    EXPECT_EQ(session.property("NEXTCLICKED"), "yesProbe");  // [Prop] argument is formatted
    EXPECT_EQ(session.property("CHK"), "agreed");           // CheckBox table Value
    ASSERT_EQ(windows->dialogs_shown.size(), 2u);
    EXPECT_EQ(windows->dialogs_shown[0], "Probe Setup");  // formatted Title
    EXPECT_EQ(windows->dialogs_shown[1], "Second");
}

TEST_F(MsiUiEngine, ControlConditionsFollowProperties) {
    InstallerUi ui = make_ui();
    windows->script.push_back([](FakeWindows& f, u64 d) {
        EXPECT_FALSE(f.wins[f.by_text(d, "&Next")].enabled);  // NOT CHK -> Disable
        return UiInput{UiInput::Kind::Activated, f.by_text(d, "I agree"), {}};
    });
    windows->script.push_back([](FakeWindows& f, u64 d) {
        EXPECT_TRUE(f.wins[f.by_text(d, "&Next")].enabled);  // CHK -> Enable
        EXPECT_TRUE(f.wins[f.by_text(d, "I agree")].checked);
        return UiInput{UiInput::Kind::Activated, f.by_text(d, "I agree"), {}};
    });
    windows->script.push_back([this](FakeWindows& f, u64 d) {
        EXPECT_FALSE(f.wins[f.by_text(d, "&Next")].enabled);
        EXPECT_FALSE(session.has_property("CHK"));  // unchecked: the property is null
        return UiInput{UiInput::Kind::Activated, f.by_text(d, "&Next"), {}};  // disabled: ignored
    });
    windows->click("Cancel");
    windows->click("Yes");
    EXPECT_EQ(ui.run_dialog(session, "Welcome"), kActionUserExit);
    EXPECT_FALSE(session.has_property("NEXTCLICKED"));
}

TEST_F(MsiUiEngine, SpawnDialogReturnsToTheParent) {
    InstallerUi ui = make_ui();
    windows->click("Cancel");
    windows->click("No");  // EndDialog Return: back to Welcome
    windows->click("I agree");
    windows->click("&Next");
    windows->click("Finish");
    EXPECT_EQ(ui.run_dialog(session, "Welcome"), kActionSuccess);
    ASSERT_EQ(windows->dialogs_shown.size(), 3u);
    EXPECT_EQ(windows->dialogs_shown[1], "Sure?");
}

TEST_F(MsiUiEngine, BoundControlsSetPropertiesAndRefreshText) {
    InstallerUi ui = make_ui();
    windows->change("", "toolongvalue");  // the Edit window has empty text
    windows->script.push_back([this](FakeWindows& f, u64 d) {
        EXPECT_EQ(session.property("EDITP"), "toolo");  // {5} limit
        EXPECT_NE(f.by_text(d, "Edit=toolo"), 0u);      // dependent Text re-formatted
        // RadioButton children: Beta (CHOICE=b) checked at creation.
        EXPECT_TRUE(f.wins[f.by_text(d, "Beta")].checked);
        EXPECT_FALSE(f.wins[f.by_text(d, "&Alpha")].checked);
        return UiInput{UiInput::Kind::Changed, f.by_text(d, "&Alpha"), {}};
    });
    windows->script.push_back([this](FakeWindows& f, u64 d) {
        EXPECT_EQ(session.property("CHOICE"), "a");
        EXPECT_TRUE(f.wins[f.by_text(d, "&Alpha")].checked);
        return UiInput{UiInput::Kind::Close, 0, {}};  // Esc = the cancel control
    });
    windows->click("Yes");
    EXPECT_EQ(ui.run_dialog(session, "Welcome"), kActionUserExit);
}

TEST_F(MsiUiEngine, LayoutUsesInstallerUnitsAndTextStyles) {
    InstallerUi ui = make_ui();
    windows->script.push_back([](FakeWindows& f, u64 d) {
        const auto& dlg = f.wins[d];
        EXPECT_EQ(dlg.cls, "MsiDialogCloseClass");
        EXPECT_EQ(dlg.rect.w, 493);  // 370 * 16 / 12
        EXPECT_EQ(dlg.rect.h, 360);
        const auto& next = f.wins[f.by_text(d, "&Next")];
        EXPECT_EQ(next.cls, "Button");
        EXPECT_EQ(next.rect, (PixelRect{314, 324, 74, 22}));
        EXPECT_EQ(next.style & 0x1u, 0x1u);  // BS_DEFPUSHBUTTON: Control_Default
        const auto& title = f.wins[f.by_text(d, "Hello Probe")];
        EXPECT_TRUE(title.font.has_value());
        if (!title.font) return UiInput{UiInput::Kind::Close, 0, {}};
        EXPECT_EQ(title.font->height, -16);  // Tahoma 12 (probe: -16)
        EXPECT_EQ(title.font->weight, 700);
        EXPECT_TRUE(title.font->underline);
        EXPECT_EQ(title.ex & 0x20u, 0x20u);  // Transparent -> WS_EX_TRANSPARENT
        const auto& check = f.wins[f.by_text(d, "I agree")];
        EXPECT_TRUE(check.font.has_value());
        if (check.font) EXPECT_EQ(check.font->height, -11);  // DefaultUIFont Tahoma 8 (probe: -11)
        // A RadioButton title's style prefix is stripped and sets that
        // button's font (probe msi_ui_layout: '{\\Big}Delta' -> 'Delta', -16 w700 ul1).
        const auto& beta = f.wins[f.by_text(d, "Beta")];
        EXPECT_TRUE(beta.font.has_value());
        if (beta.font) {
            EXPECT_EQ(beta.font->height, -16);
            EXPECT_EQ(beta.font->weight, 700);
            EXPECT_TRUE(beta.font->underline);
        }
        const auto& alpha = f.wins[f.by_text(d, "&Alpha")];
        if (alpha.font) EXPECT_EQ(alpha.font->height, -11);  // no prefix: the group's font
        return UiInput{UiInput::Kind::Close, 0, {}};
    });
    windows->click("Yes");
    EXPECT_EQ(ui.run_dialog(session, "Welcome"), kActionUserExit);
}

TEST_F(MsiUiEngine, ErrorDialogShowsOnlyTheMessageButtons) {
    InstallerUi ui = make_ui();
    Record r(0);
    r.fields[0].kind = Record::Kind::String;
    r.fields[0].text = u"Disk is full";
    windows->script.push_back([](FakeWindows& f, u64 d) {
        EXPECT_NE(f.by_text(d, "Disk is full"), 0u);
        EXPECT_TRUE(f.wins[f.by_text(d, "Yes")].visible);
        EXPECT_TRUE(f.wins[f.by_text(d, "No")].visible);
        EXPECT_FALSE(f.wins[f.by_text(d, "OK")].visible);
        EXPECT_FALSE(f.wins[f.by_text(d, "Cancel")].visible);
        return UiInput{UiInput::Kind::Activated, f.by_text(d, "No"), {}};
    });
    EXPECT_EQ(ui.message(session, kMsgError | 0x4 /*MB_YESNO*/, r), 7 /*IDNO*/);
    // The icon is the standard warning icon for an error message.
    bool warned = false;
    for (auto& [h, w] : windows->wins) warned = warned || w.icon == 32515;
    EXPECT_TRUE(warned);
}

TEST_F(MsiUiEngine, ModelessProgressDialogFollowsMessages) {
    InstallerUi ui = make_ui();
    EXPECT_EQ(ui.run_dialog(session, "ProgressDlg"), kActionSuccess);  // modeless: returns at once
    EXPECT_EQ(ui.current_dialog(), "ProgressDlg");
    const u64 d = windows->live_dialog();
    Record start(3);
    start.fields[1] = {Record::Kind::String, 0, u"InstallFiles", {}, 0};
    start.fields[2] = {Record::Kind::String, 0, u"Copying new files", {}, 0};
    EXPECT_EQ(ui.message(session, kMsgActionStart, start), 0);
    EXPECT_NE(windows->by_text(d, "Copying new files"), 0u);
    Record reset(4);
    for (u32 i = 1; i <= 4; ++i) reset.fields[i].kind = Record::Kind::Integer;
    reset.fields[2].integer = 1000;
    EXPECT_EQ(ui.message(session, kMsgProgress, reset), 0);
    Record step(2);
    step.fields[1] = {Record::Kind::Integer, 2, {}, {}, 0};
    step.fields[2] = {Record::Kind::Integer, 250, {}, {}, 0};
    EXPECT_EQ(ui.message(session, kMsgProgress, step), 0);
    u64 bar = 0;
    for (auto& [h, w] : windows->wins)
        if (w.cls == "msctls_progress32" && !w.destroyed) bar = h;
    ASSERT_NE(bar, 0u);
    EXPECT_EQ(windows->wins[bar].pos, 250u);
    EXPECT_EQ(windows->wins[bar].range, 1000u);
    Record data(1);
    data.fields[0] = {Record::Kind::String, 0, u"File: [1]", {}, 0};
    data.fields[1] = {Record::Kind::String, 0, u"a.dll", {}, 0};
    EXPECT_EQ(ui.message(session, kMsgActionData, data), 0);
    EXPECT_NE(windows->by_text(d, "File: a.dll"), 0u);
    EXPECT_EQ(ui.message(session, kMsgTerminate, Record(0)), 0);
    EXPECT_TRUE(windows->wins[d].destroyed);
}

TEST_F(MsiUiEngine, LevelsGateDialogs) {
    InstallerUi none = make_ui(sdk::INSTALLUILEVEL_NONE);
    EXPECT_EQ(none.run_dialog(session, "Welcome"), kActionNotCalled);
    InstallerUi reduced = make_ui(sdk::INSTALLUILEVEL_REDUCED);
    EXPECT_EQ(reduced.run_dialog(session, "Welcome"), kActionNotCalled);  // modal: not shown
    EXPECT_EQ(reduced.run_dialog(session, "ProgressDlg"), kActionSuccess);
    InstallerUi full = make_ui();
    EXPECT_EQ(full.run_dialog(session, "NoSuchDialog"), kActionNotCalled);
    // Basic UI: errors are message boxes.
    InstallerUi basic = make_ui(sdk::INSTALLUILEVEL_BASIC);
    Record r(0);
    r.fields[0] = {Record::Kind::String, 0, u"oops", {}, 0};
    windows->box_answer = 4;
    EXPECT_EQ(basic.message(session, kMsgError | 0x5, r), 4);
    ASSERT_EQ(windows->boxes.size(), 1u);
    EXPECT_EQ(std::get<0>(windows->boxes[0]), "oops");
    EXPECT_EQ(std::get<2>(windows->boxes[0]), 0x5u);
    InstallerUi progress_only = make_ui(sdk::INSTALLUILEVEL_BASIC | sdk::INSTALLUILEVEL_PROGRESSONLY);
    EXPECT_EQ(progress_only.message(session, kMsgError, r), 0);
}

// ---- Pure helpers -----------------------------------------------------------------------

TEST(MsiUiModel, ControlEventSelectionRules) {
    auto eval = [](const std::string& c) -> std::optional<bool> { return c == "1"; };
    // Blank conditions run only when nothing else is true; then the largest Ordering.
    std::vector<ControlEventRow> rows = {{"A", "1", "", 1}, {"B", "2", "", 5}, {"C", "3", "0", 3}};
    auto s = select_control_events(rows, eval);
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].event, "B");
    rows.push_back({"D", "4", "1", 2});
    s = select_control_events(rows, eval);
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].event, "D");
    // At most one NewDialog/SpawnDialog: the largest Ordering; others kept in order.
    rows = {{"NewDialog", "X", "1", 1}, {"[P]", "v", "1", 2}, {"SpawnDialog", "Y", "1", 3}, {"DoAction", "Z", "1", 4}};
    s = select_control_events(rows, eval);
    ASSERT_EQ(s.size(), 3u);
    EXPECT_EQ(s[0].event, "[P]");
    EXPECT_EQ(s[1].event, "SpawnDialog");
    EXPECT_EQ(s[2].event, "DoAction");
}

TEST(MsiUiModel, StyledTextPrefixes) {
    std::map<std::string, TextStyleRow> styles = {{"Big", {}}, {"Small", {}}};
    auto t = split_styled_text("{\\Big}{&Small}{80}Hello {x}", styles);
    EXPECT_EQ(t.style, "Small");
    EXPECT_EQ(t.limit, 80);
    EXPECT_EQ(t.text, "Hello {x}");
    t = split_styled_text("{\\Missing}text", styles);
    EXPECT_EQ(t.style, "");
    EXPECT_EQ(t.text, "{\\Missing}text");
    t = split_styled_text("{-3}x", styles);
    EXPECT_TRUE(t.bad_limit);
}

TEST(MsiUiModel, MaskTemplate) {
    // Learn MaskedEdit example: 12345<### -%%%%%%%>@@@@@
    auto m = parse_mask("12345<### -%%%%%%%>@@@@@");
    EXPECT_EQ(m.hidden_prefix, "12345");
    EXPECT_EQ(m.hidden_suffix, "@@@@@");
    ASSERT_EQ(m.segments.size(), 3u);
    EXPECT_EQ(m.segments[0].kind, MaskSegment::Kind::Digit);
    EXPECT_EQ(m.segments[0].length, 3);
    EXPECT_EQ(m.segments[1].text, " -");
    EXPECT_EQ(m.segments[2].kind, MaskSegment::Kind::AltDigit);
    EXPECT_EQ(m.segments[2].length, 7);
    m = parse_mask("##=##");
    ASSERT_EQ(m.segments.size(), 3u);
    EXPECT_EQ(m.segments[0].length, 3);
    EXPECT_EQ(m.segments[1].text, "-");
}

TEST(MsiUiModel, FormatSizeMatchesWindows) {
    // The probe's D2 dialog: these Text values with FormatSize, in order.
    const char* inputs[] = {"0", "1", "2", "3", "20", "2047", "2048", "20479", "20480", "204800",
                            "2097151", "2097152", "20971519", "20971520", "214748364", "12345678", "x12", "-4"};
    // UIText of the probe package: bytes=B, KB, MB, GB.
    std::map<std::string, std::string> ui = {{"bytes", "B"}, {"KB", "KB"}, {"MB", "MB"}, {"GB", "GB"}};
    const auto lines = expected_lines("msi_ui_layout64.expected");
    std::vector<std::string> want;
    bool in_d2 = false;
    const std::regex text_re("text '([^']*)'");
    for (const std::string& l : lines) {
        if (l.rfind("dialog D2", 0) == 0) in_d2 = true;
        else if (in_d2 && l.rfind("  child", 0) == 0) {
            std::smatch m;
            if (std::regex_search(l, m, text_re)) want.push_back(m[1]);
        } else if (in_d2) break;
    }
    ASSERT_EQ(want.size(), std::size(inputs));
    for (usize i = 0; i < want.size(); ++i)
        EXPECT_EQ(format_size_text(std::strtoll(inputs[i], nullptr, 10), ui, nullptr).value_or("?"), want[i]) << inputs[i];
    std::string missing;
    EXPECT_FALSE(format_size_text(12345678, {{"KB", "KB"}}, &missing).has_value());
    EXPECT_EQ(missing, "MB");  // probe: "MB is missing from the UIText Table"
}

TEST(MsiUiModel, FontHeightsMatchWindows) {
    EXPECT_EQ(font_from_style({"Tahoma", 8, {}, 0}, 96).height, -11);
    EXPECT_EQ(font_from_style({"Tahoma", 10, {}, 0}, 96).height, -13);
    const FontSpec big = font_from_style({"Tahoma", 12, 255u, 5}, 96);
    EXPECT_EQ(big.height, -16);
    EXPECT_EQ(big.weight, 700);
    EXPECT_TRUE(big.underline);
    EXPECT_FALSE(big.italic);
}

// The probe's D1 controls (the inputs it authored: name, type, x, y, w, h, attributes).
struct ProbeControl {
    const char* type;
    i32 x, y, w, h;
    u32 attr;
};
const ProbeControl kProbeControls[] = {
    {"PushButton", 10, 5, 56, 17, 3}, {"PushButton", 70, 5, 56, 17, 3}, {"PushButton", 130, 5, 22, 17, 3 | 0x00040000 | 0x00100000},
    {"PushButton", 156, 5, 22, 17, 3 | 0x00080000 | 0x00200000},
    {"PushButton", 182, 5, 56, 17, 1 | 0x00800000},  // the probe disables it through ControlCondition
    {"Text", 10, 26, 120, 10, 3}, {"Edit", 10, 40, 100, 15, 7}, {"Edit", 120, 40, 100, 30, 7 | 0x00010000},
    {"Edit", 230, 40, 100, 15, 7 | 0x00200000}, {"PathEdit", 10, 75, 150, 15, 7}, {"MaskedEdit", 170, 75, 150, 15, 7},
    {"CheckBox", 10, 95, 100, 12, 3}, {"CheckBox", 120, 95, 60, 17, 3 | 0x00020000},
    {"RadioButtonGroup", 10, 112, 150, 40, 3}, {"RadioButtonGroup", 170, 112, 150, 40, 3 | 0x01000000},
    {"ComboBox", 10, 157, 100, 50, 7}, {"ComboBox", 120, 157, 100, 50, 7 | 0x00020000 | 0x00010000},
    {"ListBox", 230, 157, 100, 40, 7}, {"DirectoryCombo", 10, 202, 100, 50, 7 | 0x00020000},
    {"VolumeSelectCombo", 340, 202, 60, 50, 7 | 0x00020000},
    {"ListView", 340, 157, 60, 40, 7}, {"DirectoryList", 120, 202, 100, 40, 7}, {"VolumeCostList", 230, 202, 100, 40, 7 | 0x00020000},
    {"SelectionTree", 10, 247, 150, 40, 7}, {"ProgressBar", 170, 247, 100, 10, 1}, {"Bitmap", 280, 247, 40, 20, 1},
    {"Icon", 330, 247, 16, 16, 1 | 0x00100000 | 0x00200000}, {"Line", 10, 292, 300, 0, 1}, {"GroupBox", 10, 296, 100, 30, 1},
    {"ScrollableText", 230, 296, 100, 30, 7 | 0x00000004}, {"Hyperlink", 340, 296, 60, 12, 3},
    // Text with Transparent|NoPrefix and NoWrap|FormatSize: Windows draws
    // these styles itself; Vitreo maps them to SS_NOPREFIX / no-wrap
    // styles, so only class, ex-style and rect are compared below.
};

TEST(MsiUiModel, ControlWindowsMatchTheMeasuredLayout) {
    struct Child {
        std::string cls;
        u32 style, ex;
        PixelRect rect;
    };
    std::vector<Child> measured;
    const std::regex re("^  child id -?\\d+ class '([^']*)' style 0x([0-9a-f]+) ex 0x([0-9a-f]+) rect (-?\\d+),(-?\\d+),(\\d+),(\\d+)");
    for (const std::string& l : expected_lines("msi_ui_layout64.expected")) {
        if (l.rfind("previewbillboard", 0) == 0) break;  // the first D1 dump only
        std::smatch m;
        if (std::regex_search(l, m, re))
            measured.push_back({m[1], static_cast<u32>(std::stoul(m[2], nullptr, 16)), static_cast<u32>(std::stoul(m[3], nullptr, 16)),
                                {std::stoi(m[4]), std::stoi(m[5]), std::stoi(m[6]), std::stoi(m[7])}});
    }
    ASSERT_GT(measured.size(), 20u);
    const UnitScale scale{16};
    for (const ProbeControl& p : kProbeControls) {
        ControlRow row;
        row.type = p.type;
        row.x = p.x, row.y = p.y, row.width = p.w, row.height = p.h, row.attributes = p.attr;
        const bool is_default = p.x == 70 && std::string(p.type) == "PushButton";  // PBD: Control_Default
        auto spec = control_window_spec(row, scale, is_default);
        ASSERT_TRUE(spec.has_value()) << p.type;
        const Child* hit = nullptr;
        for (const Child& c : measured)
            if (c.rect.x == spec->rect.x && c.rect.y == spec->rect.y && c.cls == narrow(spec->class_name)) hit = &c;
        ASSERT_NE(hit, nullptr) << p.type << " at " << spec->rect.x << "," << spec->rect.y;
        EXPECT_EQ(spec->style | 0x10000000u, hit->style) << p.type;  // WS_VISIBLE is set by showing
        EXPECT_EQ(spec->ex_style, hit->ex) << p.type;
        EXPECT_EQ(spec->rect.w, hit->rect.w) << p.type;
        // Combo and list boxes size their own height; the rest keep the scaled height.
        if (narrow(spec->class_name) != "ComboBox" && narrow(spec->class_name) != "ListBox") EXPECT_EQ(spec->rect.h, hit->rect.h) << p.type;
    }
    // A billboard has no window of its own.
    ControlRow bb;
    bb.type = "Billboard";
    EXPECT_FALSE(control_window_spec(bb, scale, false).has_value());
    // A BBControl converts summed units (probe: billboard at 120,296 + 2,2 -> 162,397).
    ControlRow bt;
    bt.type = "Text";
    bt.x = 2, bt.y = 2, bt.width = 60, bt.height = 10, bt.attributes = 1;
    auto spec = control_window_spec(bt, scale, false, 120, 296);
    EXPECT_EQ(spec->rect, (PixelRect{162, 397, 80, 13}));
}

TEST(MsiUiModel, DialogWindowMatchesTheMeasuredLayout) {
    DialogRow d;
    d.width = 410, d.height = 330, d.attributes = 3, d.cancel = "PB";
    auto s = dialog_window_spec(d, UnitScale{16});
    EXPECT_EQ(narrow(s.class_name), "MsiDialogCloseClass");
    EXPECT_EQ(s.client_w, 546);  // probe: client 546x440
    EXPECT_EQ(s.client_h, 440);
    EXPECT_EQ(s.style | 0x10000000u, 0x14c80000u);
    EXPECT_EQ(s.ex_style, 0x00040100u);
    d.cancel.clear();
    d.width = 300, d.height = 300;
    s = dialog_window_spec(d, UnitScale{16});
    EXPECT_EQ(narrow(s.class_name), "MsiDialogNoCloseClass");  // probe D2
    EXPECT_EQ(s.client_w, 400);
}

TEST(MsiUiModel, InternalUiLevelRules) {
    // Replays the probe's MsiSetInternalUI sequence (each change followed by
    // a NOCHANGE query, as the probe did).
    u32 stored = sdk::INSTALLUILEVEL_DEFAULT;
    const std::regex re("internal set 0x([0-9a-f]+) -> ret 0x([0-9a-f]+) now 0x([0-9a-f]+)");
    usize n = 0;
    for (const std::string& l : expected_lines("msi_ui_api64.expected")) {
        std::smatch m;
        if (!std::regex_search(l, m, re)) continue;
        const u32 req = static_cast<u32>(std::stoul(m[1], nullptr, 16));
        const auto change = apply_internal_ui_level(stored, req);
        EXPECT_EQ(change.returned, std::stoul(m[2], nullptr, 16)) << l;
        stored = change.new_level;
        const auto query = apply_internal_ui_level(stored, 0);
        EXPECT_EQ(query.returned, std::stoul(m[3], nullptr, 16)) << l;
        stored = query.new_level;
        ++n;
    }
    EXPECT_GT(n, 40u);
}

// ---- Exports through the fixture ------------------------------------------------------------

class MsiUi : public MsiTest {
protected:
    void SetUp() override {
        MsiTest::SetUp();
        reset_ui_process_state(mgr);
    }
    void TearDown() override { reset_ui_process_state(mgr); }
    static constexpr GuestAddr STR = DATA + 0x8000;
    void write_ansi(GuestAddr at, const std::string& s) {
        for (usize i = 0; i < s.size(); ++i) ASSERT_TRUE(memory.write_u8(at + i, static_cast<u8>(s[i])).is_ok());
        ASSERT_TRUE(memory.write_u8(at + s.size(), 0).is_ok());
    }
    void write_ptr(GuestAddr at, u64 v) {
        if (x64()) ASSERT_TRUE(memory.write_u64(at, v).is_ok());
        else ASSERT_TRUE(memory.write_u32(at, static_cast<u32>(v)).is_ok());
    }
};

TEST_P(MsiUi, SetInternalUIExchangesLevelAndOwner) {
    EXPECT_EQ(call("MsiSetInternalUI", {I(0), I(0)}), 1u);  // "When Msi.dll is loaded ... DEFAULT"
    // Probe owner lines: set1..after-null.
    write_ptr(OUT, 0x1234);
    EXPECT_EQ(call("MsiSetInternalUI", {I(0), I(OUT)}), 1u);
    EXPECT_EQ(read_handle(OUT), 0u);
    write_ptr(OUT, 0x5678);
    EXPECT_EQ(call("MsiSetInternalUI", {I(2), I(OUT)}), 1u);
    EXPECT_EQ(read_handle(OUT), 0x1234u);
    write_ptr(OUT, 0x9abc);
    EXPECT_EQ(call("MsiSetInternalUI", {I(7), I(OUT)}), 0u);
    EXPECT_EQ(read_handle(OUT), 0x9abcu);  // untouched, owner unchanged
    write_ptr(OUT, 0);
    EXPECT_EQ(call("MsiSetInternalUI", {I(0), I(OUT)}), 2u);
    EXPECT_EQ(read_handle(OUT), 0x5678u);
    write_ptr(OUT, 0);
    EXPECT_EQ(call("MsiSetInternalUI", {I(0), I(OUT)}), 2u);
    EXPECT_EQ(read_handle(OUT), 0u);
    EXPECT_EQ(ui_internal_level(mgr), 2u);
}

TEST_P(MsiUi, InternalUiFollowsTheEngineLevel) {
    EXPECT_EQ(call("MsiSetInternalUI", {I(4 | 0x20), I(0)}), 1u);
    EXPECT_EQ(mgr.engine_ui_level(), 0x24u);  // what MsiInstallProduct / MsiOpenPackage sessions use
    InstallerUi ui(mgr, nullptr, UiHooks{}, 0);
    EXPECT_EQ(ui.level(), 0x24u);
    install_internal_ui(mgr, std::make_shared<FakeWindows>());
    EXPECT_EQ(mgr.engine_services().ui != nullptr, true);
    install_internal_ui(mgr, nullptr);
    EXPECT_EQ(mgr.engine_services().ui, nullptr);
}

TEST_P(MsiUi, ExternalHandlerSlots) {
    const u64 hA = 0x401000, hW = 0x402000, hW2 = 0x403000, hR = 0x404000;
    // Probe extW1..extW6 / extR1..extR5.
    EXPECT_EQ(call("MsiSetExternalUIW", {I(hW), I(2), I(1)}), 0u);
    EXPECT_EQ(call("MsiSetExternalUIW", {I(hW2), I(2), I(2)}), hW);
    EXPECT_EQ(call("MsiSetExternalUIA", {I(hA), I(2), I(3)}), 0u);
    EXPECT_EQ(call("MsiSetExternalUIW", {I(hW), I(2), I(4)}), 0u);
    EXPECT_EQ(call("MsiSetExternalUIA", {I(0), I(0), I(0)}), 0u);
    EXPECT_EQ(call("MsiSetExternalUIW", {I(0), I(0), I(0)}), 0u);
    EXPECT_EQ(call("MsiSetExternalUIW", {I(hW), I(0), I(0)}), 0u);
    EXPECT_EQ(call("MsiSetExternalUIW", {I(0), I(0), I(0)}), hW);
    write_ptr(OUT, 0x77);
    EXPECT_EQ(call("MsiSetExternalUIRecord", {I(hR), I(2), I(0), I(OUT)}), 0u);
    EXPECT_EQ(read_handle(OUT), 0u);
    write_ptr(OUT, 0x77);
    EXPECT_EQ(call("MsiSetExternalUIRecord", {I(hR), I(0), I(0), I(OUT)}), 0u);
    EXPECT_EQ(read_handle(OUT), hR);
    EXPECT_EQ(call("MsiSetExternalUIRecord", {I(0), I(0), I(0), I(0)}), 0u);
    write_ptr(OUT, 0x77);
    EXPECT_EQ(call("MsiSetExternalUIRecord", {I(0), I(2), I(0), I(OUT)}), 0u);
    EXPECT_EQ(read_handle(OUT), 0u);
    {
        CustomActionUiScope in_ca;
        EXPECT_EQ(call("MsiSetExternalUIRecord", {I(hR), I(2), I(0), I(0)}), 120u);  // Learn
    }
}

TEST_P(MsiUi, EnableLogArguments) {
    write_wide(STR, u"C:\\x.log");
    EXPECT_EQ(call("MsiEnableLogW", {I(0), I(0), I(0)}), 0u);
    EXPECT_EQ(call("MsiEnableLogW", {I(2), I(0), I(0)}), 0u);
    EXPECT_EQ(call("MsiEnableLogW", {I(0), I(STR), I(0)}), 87u);
    EXPECT_EQ(call("MsiEnableLogW", {I(0xFFFFFFFF), I(STR), I(0xFFFFFFFF)}), 0u);
    auto st = ui_process_state(mgr);
    EXPECT_TRUE(st.log.enabled);
    EXPECT_EQ(st.log.path, "C:\\x.log");
    EXPECT_EQ(st.log.mode, 0xFFFFFFFFu);
    write_ansi(STR + 0x100, "C:\\a.log");
    EXPECT_EQ(call("MsiEnableLogA", {I(2), I(STR + 0x100), I(1)}), 0u);
    st = ui_process_state(mgr);
    EXPECT_EQ(st.log.path, "C:\\a.log");
    EXPECT_EQ(st.log.attributes, 1u);
    EXPECT_EQ(call("MsiEnableLogW", {I(0), I(0), I(0)}), 0u);
    EXPECT_FALSE(ui_process_state(mgr).log.enabled);
}

TEST_P(MsiUi, PreviewErrorCodes) {
    const fs::path root = fs::temp_directory_path() / ("vitreo_msi_ui_" + std::to_string(::getpid()) + (x64() ? "_64" : "_32"));
    fs::remove_all(root);
    fs::create_directories(root);
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
    mgr.set_vfs(&vfs);
    write_wide(STR, u"C:\\p.msi");
    ASSERT_EQ(call("MsiOpenDatabaseW", {I(STR), I(3 /*MSIDBOPEN_CREATE*/), I(OUT)}), 0u);
    const u32 db = static_cast<u32>(read_int(OUT));
    // Probe preview lines.
    write_int(OUT2, 0x55);
    EXPECT_EQ(call("MsiEnableUIPreview", {I(0), I(OUT2)}), 6u);
    EXPECT_EQ(read_int(OUT2), 0x55);
    EXPECT_EQ(call("MsiEnableUIPreview", {I(db), I(0)}), 87u);
    ASSERT_EQ(call("MsiEnableUIPreview", {I(db), I(OUT2)}), 0u);
    const u32 pv = static_cast<u32>(read_int(OUT2));
    EXPECT_NE(pv, 0u);
    EXPECT_EQ(call("MsiEnableUIPreview", {I(db), I(OUT3)}), 14u);
    write_wide(STR + 0x100, u"Nope");
    write_wide(STR + 0x200, u"");
    EXPECT_EQ(call("MsiPreviewDialogW", {I(pv), I(STR + 0x100)}), 1626u);
    EXPECT_EQ(call("MsiPreviewDialogW", {I(pv), I(0)}), 1626u);
    EXPECT_EQ(call("MsiPreviewDialogW", {I(pv), I(STR + 0x200)}), 0u);
    EXPECT_EQ(call("MsiPreviewDialogW", {I(db), I(STR + 0x100)}), 6u);
    EXPECT_EQ(call("MsiPreviewBillboardW", {I(pv), I(STR + 0x100), I(STR + 0x100)}), 1627u);
    EXPECT_EQ(call("MsiPreviewBillboardW", {I(pv), I(STR + 0x100), I(0)}), 0u);
    EXPECT_EQ(call("MsiPreviewBillboardW", {I(pv), I(0), I(0)}), 87u);
    EXPECT_EQ(call("MsiPreviewBillboardW", {I(db), I(STR + 0x100), I(0)}), 6u);
    EXPECT_EQ(call("MsiCloseHandle", {I(pv)}), 0u);
    EXPECT_EQ(call("MsiEnableUIPreview", {I(db), I(OUT3)}), 0u);  // the first preview ended
    (void)call("MsiCloseHandle", {I(static_cast<u32>(read_int(OUT3)))});
    (void)call("MsiCloseHandle", {I(db)});
    fs::remove_all(root);
}

INSTANTIATE_MSI_BITNESS(MsiUi);

// ---- Message routing -------------------------------------------------------------------------

class FakeGuest : public UiGuest {
public:
    std::vector<std::string> calls;
    i32 record_answer{0}, string_answer{0};
    MsiManager* mgr{nullptr};
    i32 call_string_handler(const ExternalStringHandler& h, u32 type, const std::u16string* text) override {
        calls.push_back("STR " + std::to_string(type >> 24) + " " + (text ? "'" + narrow(*text) + "'" : "(null)") +
                        (h.unicode ? " W" : " A"));
        return string_answer;
    }
    i32 call_record_handler(const ExternalRecordHandler&, u32 type, u32 record) override {
        std::string fields;
        if (record != 0) {
            auto r = mgr->handles().get<Record>(record);
            fields = r ? " n=" + std::to_string(r->count()) : " bad";
        }
        calls.push_back("REC " + std::to_string(type >> 24) + " h" + (record ? "1" : "0") + fields);
        return record_answer;
    }
};

class MsiUiRouting : public MsiTest {
protected:
    void SetUp() override {
        MsiTest::SetUp();
        reset_ui_process_state(mgr);
    }
    void TearDown() override { reset_ui_process_state(mgr); }
};

TEST_P(MsiUiRouting, RecordThenStringThenInternal) {
    auto guest = std::make_shared<FakeGuest>();
    guest->mgr = &mgr;
    set_ui_guest(mgr, guest);
    ASSERT_EQ(call("MsiSetExternalUIRecord", {I(0x401000), I(log_mode_bit(kMsgCommonData) | log_mode_bit(kMsgInitialize)),
                                              I(0x11), I(0)}),
              0u);
    (void)call("MsiSetExternalUIW", {I(0x402000), I(0xFFFFFFFF), I(0x22)});
    Record common(3);
    for (u32 i = 1; i <= 3; ++i) common.fields[i].kind = Record::Kind::Integer;
    common.fields[2].integer = 1033;
    common.fields[3].integer = 1252;
    // The probe's COMMONDATA string: "1: 0 2: 1033 3: 1252 ".
    const RecordFormatter fmt = [](const Record& r) { return Session::format_record_without_session(r); };
    EXPECT_EQ(route_message(mgr, nullptr, nullptr, kMsgCommonData, &common, fmt), 0);
    ASSERT_EQ(guest->calls.size(), 2u);
    EXPECT_EQ(guest->calls[0], "REC 11 h1 n=3");
    EXPECT_EQ(guest->calls[1], "STR 11 '1: 0 2: 1033 3: 1252 ' W");
    guest->calls.clear();
    // INITIALIZE carries no record: hRecord 0, a null string.
    EXPECT_EQ(route_message(mgr, nullptr, nullptr, kMsgInitialize, nullptr, fmt), 0);
    ASSERT_EQ(guest->calls.size(), 2u);
    EXPECT_EQ(guest->calls[0], "REC 12 h0");
    EXPECT_EQ(guest->calls[1], "STR 12 (null) W");
    guest->calls.clear();
    // A record handler that answers suppresses the string handler (probe uninstall run).
    guest->record_answer = 1;
    EXPECT_EQ(route_message(mgr, nullptr, nullptr, kMsgCommonData, &common, fmt), 1);
    ASSERT_EQ(guest->calls.size(), 1u);
    guest->calls.clear();
    // Filtered out of the record handler: only the string handler (probe install 2).
    Record start(1);
    start.fields[1] = {Record::Kind::String, 0, u"SetP", {}, 0};
    EXPECT_EQ(route_message(mgr, nullptr, nullptr, kMsgActionStart, &start, fmt), 0);
    ASSERT_EQ(guest->calls.size(), 1u);
    EXPECT_EQ(guest->calls[0].rfind("STR 8", 0), 0u);
    // The record handle is closed after the call.
    EXPECT_FALSE(mgr.handles().contains(1));
}

INSTANTIATE_MSI_BITNESS(MsiUiRouting);

// ---- The user32 window system ---------------------------------------------------------------

class FakeGuestApi : public UiGuestApi {
public:
    FakeGuestApi(GuestMemory& m, bool x64) : mem(m), x64(x64) {}
    struct Call {
        std::string fn;
        std::vector<u64> args;
    };
    std::vector<Call> calls;
    GuestMemory& mem;
    bool x64;
    GuestAddr next{0x40000};
    u64 next_handle{0x1000};
    u64 call(std::string_view, std::string_view fn, const std::vector<u64>& args) override {
        calls.push_back({std::string(fn), args});
        if (fn == "CreateWindowExW" || fn == "RegisterClassExW" || fn == "CreateFontIndirectW") return next_handle++;
        if (fn == "GetDeviceCaps") return 96;
        if (fn == "MsgWaitForMultipleObjects") return 0;
        return 0;
    }
    GuestAddr alloc(u32 bytes) override {
        const GuestAddr a = next;
        next += (bytes + 15) & ~15u;
        std::vector<u8> zero(bytes, 0);
        (void)mem.write_bytes(a, zero.data(), zero.size());
        return a;
    }
    void release(GuestAddr) override {}
    GuestMemory& memory() override { return mem; }
    bool is_64bit() const override { return x64; }
    GuestAddr window_proc() const override { return 0x7700; }
    std::string wide_at(u64 p) {
        std::string s;
        for (u16 c; (c = mem.read_u16(p).value_or(0)) != 0; p += 2) s.push_back(static_cast<char>(c));
        return s;
    }
};

class MsiUiWin32 : public MsiTest {};

TEST_P(MsiUiWin32, CreatesWindowsAndTranslatesNotifications) {
    auto api = std::make_shared<FakeGuestApi>(memory, x64());
    auto ws = make_user32_window_system(api);
    DialogRow row;
    row.width = 370, row.height = 270, row.attributes = 3, row.cancel = "C";
    DialogWindowSpec spec = dialog_window_spec(row, UnitScale{16});
    spec.title = u"T";
    const u64 dlg = ws->create_dialog(spec, 0);
    ASSERT_NE(dlg, 0u);
    // RegisterClassExW with the window-procedure export and DLGWINDOWEXTRA.
    const FakeGuestApi::Call* reg = nullptr;
    for (const auto& c : api->calls)
        if (c.fn == "RegisterClassExW") reg = &c;
    ASSERT_NE(reg, nullptr);
    const GuestAddr wc = reg->args[0];
    EXPECT_EQ(memory.read_u32(wc).value(), x64() ? 80u : 48u);  // cbSize (probe)
    EXPECT_EQ(x64() ? memory.read_u64(wc + 8).value() : memory.read_u32(wc + 8).value(), 0x7700u);
    EXPECT_EQ(memory.read_u32(wc + (x64() ? 20 : 16)).value(), 30u);
    const GuestAddr cls = x64() ? memory.read_u64(wc + 64).value() : memory.read_u32(wc + 40).value();
    EXPECT_EQ(api->wide_at(cls), "MsiDialogCloseClass");
    ControlRow b;
    b.type = "PushButton";
    b.x = 10, b.y = 5, b.width = 56, b.height = 17, b.attributes = 3, b.text = "OK";
    auto bspec = control_window_spec(b, UnitScale{16}, true);
    bspec->text = u"OK";
    const u64 button = ws->create_control(dlg, *bspec);
    const auto& cw = api->calls.back();
    ASSERT_EQ(cw.fn, "CreateWindowExW");
    EXPECT_EQ(api->wide_at(cw.args[1]), "Button");
    EXPECT_EQ(api->wide_at(cw.args[2]), "OK");
    EXPECT_EQ(cw.args[3], bspec->style);
    EXPECT_EQ(cw.args[4], 13u);
    EXPECT_EQ(cw.args[6], 74u);
    EXPECT_EQ(cw.args[8], dlg);
    // WM_COMMAND BN_CLICKED from the button, through the window procedure.
    handlers["__ui_proc"] = [](Marshaler& m) { ui_window_proc(m); };
    EXPECT_EQ(call("__ui_proc", {I(dlg), I(0x111), I(1000), I(button)}), 0u);
    const UiInput in = ws->wait_input(dlg, 0);
    EXPECT_EQ(in.kind, UiInput::Kind::Activated);
    EXPECT_EQ(in.window, button);
    // WM_CLOSE is the cancel control; nothing is destroyed.
    (void)call("__ui_proc", {I(dlg), I(0x10), I(0), I(0)});
    EXPECT_EQ(ws->wait_input(dlg, 0).kind, UiInput::Kind::Close);
    // Anything else goes to DefDlgProcW.
    api->calls.clear();
    (void)call("__ui_proc", {I(dlg), I(0x0F), I(0), I(0)});
    ASSERT_FALSE(api->calls.empty());
    EXPECT_EQ(api->calls.back().fn, "DefDlgProcW");
}

INSTANTIATE_MSI_BITNESS(MsiUiWin32);

}  // namespace
}  // namespace vitreo::winapi::msi_test
