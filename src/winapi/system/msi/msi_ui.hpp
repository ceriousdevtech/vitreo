#pragma once
/// @file msi_ui.hpp
/// @brief Windows Installer internal UI: UI levels, external UI handlers,
/// the dialog model built from the Dialog/Control tables, the ControlEvent
/// engine, and the Win32 window layout the dialogs are presented with.
///
/// Layers, each testable without the one below it:
///   * process state (MsiSetInternalUI / MsiSetExternalUI* / MsiEnableLog),
///     per MsiManager ("all subsequent installations initiated in the calling
///     process");
///   * message routing (route_message): external record handler, then the
///     external string handler, then the internal UI (Learn
///     MsiSetExternalUIRecord remarks; probe msi_ui_api "install" lines);
///   * the dialog model (load_dialog) and the event engine (DialogRunner):
///     ControlEvent selection and execution, ControlCondition, EventMapping
///     subscriptions, property binding, wizard sequencing, error dialogs;
///   * the Win32 view (control_window_spec): which window class, styles and
///     pixel rectangle each control gets, as measured on Windows 11
///     (tools/oracle_probe/msi_ui_layout_probe.c, *.expected);
///   * WindowSystem: the windows themselves, created through user32 in the
///     msiexec guest process. The engine never touches HWNDs directly.
///
/// Contract sources: Msi.h / MsiQuery.h / MsiDefs.h (SDK 10.0.26100; numeric
/// values generated into msi_ui_constants.inc by tools/gen_msi_ui_constants.py),
/// Microsoft Learn (Dialog, Control, ControlEvent, ControlCondition,
/// EventMapping, TextStyle, UIText, RadioButton, CheckBox, ComboBox, ListBox,
/// ListView, Billboard, BBControl tables; every control and ControlEvent page;
/// User Interface Levels; Error Dialog; Installer Units; MsiSetInternalUI,
/// MsiSetExternalUI(Record), INSTALLUI_HANDLER(_RECORD), MsiEnableLog,
/// MsiEnableUIPreview, MsiPreviewDialog, MsiPreviewBillboard), and the
/// msi_ui_* oracle probes. Gaps: .docs/contracts/msi/ui.md.

#include <vitreo/common.hpp>
#include "winapi/system/msi/msi_core.hpp"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo { class GuestMemory; }

namespace vitreo::winapi::msi {

class Session;
class Package;

/// SDK constants (generated; see the file comment).
namespace sdk {
#include "winapi/system/msi/msi_ui_constants.inc"
}  // namespace sdk

/// INSTALLLOGMODE bit of an INSTALLMESSAGE type (Msi.h:
/// `1 << (INSTALLMESSAGE_x >> 24)`).
[[nodiscard]] constexpr u32 log_mode_bit(u32 message_type) { return 1u << ((message_type >> 24) & 31u); }

/// The INSTALLUILEVEL modifier bits (ENDDIALOG|PROGRESSONLY|HIDECANCEL|
/// SOURCERESONLY|UACONLY).
inline constexpr u32 kUiLevelModifiers = sdk::INSTALLUILEVEL_ENDDIALOG | sdk::INSTALLUILEVEL_PROGRESSONLY |
                                         sdk::INSTALLUILEVEL_HIDECANCEL | sdk::INSTALLUILEVEL_SOURCERESONLY |
                                         sdk::INSTALLUILEVEL_UACONLY;

// ---- Process state ------------------------------------------------------------------

/// A guest INSTALLUI_HANDLERA/W (MsiSetExternalUIA/W share one slot: probe
/// "extA1/extW3" — setting one replaces the other).
struct ExternalStringHandler {
    GuestAddr function{0};
    bool unicode{true};
    u32 filter{0};
    GuestAddr context{0};
    bool guest_64bit{true};
};
/// A guest INSTALLUI_HANDLER_RECORD.
struct ExternalRecordHandler {
    GuestAddr function{0};
    u32 filter{0};
    GuestAddr context{0};
    bool guest_64bit{true};
};
/// MsiEnableLog's request; the engine's logger writes the file.
struct LogRequest {
    bool enabled{false};
    u32 mode{0};             ///< INSTALLLOGMODE_*
    std::string path;        ///< UTF-8 guest path
    u32 attributes{0};       ///< INSTALLLOGATTRIBUTES_*
};

/// How the UI calls guest code. The runtime supplies one built on the
/// guest-call facility (make_guest_call_ui); tests supply fakes.
class UiGuest {
public:
    virtual ~UiGuest() = default;
    /// `int WINAPI handler(LPVOID ctx, UINT type, LPCSTR/LPCWSTR text)`;
    /// @p text null passes a null pointer (INITIALIZE / TERMINATE).
    virtual i32 call_string_handler(const ExternalStringHandler& h, u32 type, const std::u16string* text) = 0;
    /// `int WINAPI handler(LPVOID ctx, UINT type, MSIHANDLE record)`.
    virtual i32 call_record_handler(const ExternalRecordHandler& h, u32 type, u32 record_handle) = 0;
};

/// The nested guest call (same shape as every other manager's GuestCall).
using UiGuestCall = std::function<u64(GuestAddr function, const u64* args, usize count, bool x64)>;
/// A UiGuest that copies handler strings to a per-thread scratch page in
/// @p memory and calls the handler through @p call.
[[nodiscard]] std::shared_ptr<UiGuest> make_guest_call_ui(GuestMemory& memory, UiGuestCall call);

class WindowSystem;

/// Per-process UI state (one per MsiManager).
struct UiProcessState {
    u32 level{sdk::INSTALLUILEVEL_DEFAULT};  ///< base level | modifier bits
    u64 owner{0};                            ///< HWND (MsiSetInternalUI phWnd)
    ExternalStringHandler string_handler;
    ExternalRecordHandler record_handler;
    LogRequest log;
};
[[nodiscard]] UiProcessState ui_process_state(MsiManager& mgr);
/// Forget @p mgr's UI state (Msi.dll load state again). The state is keyed
/// by the manager's address; a test that builds managers repeatedly resets
/// it so a new manager at a reused address starts clean.
void reset_ui_process_state(MsiManager& mgr);
/// The internal UI level MsiSetInternalUI selected (INSTALLUILEVEL_* with
/// modifiers): what MsiInstallProduct / MsiOpenPackage sessions start with.
[[nodiscard]] u32 ui_internal_level(MsiManager& mgr);
void set_ui_guest(MsiManager& mgr, std::shared_ptr<UiGuest> guest);
/// The windows of this process (the msiexec guest process's user32).
/// Without one the internal UI cannot show dialogs.
void set_ui_window_system(MsiManager& mgr, std::shared_ptr<WindowSystem> windows);
[[nodiscard]] std::shared_ptr<WindowSystem> ui_window_system(MsiManager& mgr);

/// While a custom action's code runs on this thread (msi_customaction.cpp
/// brackets call_dll_entry with it), MsiSetExternalUIRecord fails with
/// ERROR_CALL_NOT_IMPLEMENTED (Learn: "cannot be called from a custom action").
class CustomActionUiScope {
public:
    CustomActionUiScope();
    ~CustomActionUiScope();
    CustomActionUiScope(const CustomActionUiScope&) = delete;
    CustomActionUiScope& operator=(const CustomActionUiScope&) = delete;
    [[nodiscard]] static bool active();
};

// ---- Message routing ----------------------------------------------------------------

/// Format a record the way the string handler receives it (the engine's
/// MsiFormatRecord: Session::format_record).
using RecordFormatter = std::function<std::string(const Record&)>;

/// MsiProcessMessage's UI half: the external record handler (if its filter
/// has the type's bit), then — when it returned 0 — the external string
/// handler, then — when that returned 0 — @p internal (the internal UI).
/// Returns the first non-zero answer, else 0. @p record null means the
/// message carries no record (INITIALIZE / TERMINATE: the record handler
/// gets hRecord 0 and the string handler a null string; probe).
i32 route_message(MsiManager& mgr, UiHost* internal, Session* session, u32 type, const Record* record,
                  const RecordFormatter& format);

/// The installer service's per-install relay to its client (msi_service.cpp):
/// while set, route_message hands every message to it (with the text the
/// string handler would get) instead of this process's handlers and UI.
using ClientUiRelay = std::function<i32(u32 type, const Record* record, const std::string& text)>;
void set_client_ui_relay(MsiManager& mgr, ClientUiRelay relay);

/// The external handlers alone (route_message without an internal UI).
i32 dispatch_external_ui(MsiManager& mgr, u32 type, const Record* record, const RecordFormatter& format);

// ---- Text styles and geometry -----------------------------------------------------------

struct TextStyleRow {
    std::string face;
    i32 size{0};                 ///< points
    std::optional<u32> color;    ///< 0x00BBGGRR (Text controls only)
    u32 bits{0};                 ///< msidbTextStyleStyleBits*
};

/// A font as the window system creates it (LOGFONTW fields).
struct FontSpec {
    std::u16string face;
    i32 height{0};               ///< lfHeight: -MulDiv(points, dpi, 72) (probe)
    i32 weight{400};
    bool italic{false}, underline{false}, strike{false};
    [[nodiscard]] bool operator==(const FontSpec&) const = default;
};
[[nodiscard]] FontSpec font_from_style(const TextStyleRow& style, i32 dpi);

/// A Control-table Text cell split into its leading `{\Style}` / `{&Style}`
/// font prefixes and the optional `{n}` length limit (Learn: Control table,
/// Edit/ComboBox controls).
struct StyledText {
    std::string style;            ///< last style named, "" = DefaultUIFont
    std::optional<i64> limit;     ///< {n}; nullopt when absent
    bool bad_limit{false};        ///< "{-1}" / "{x}": a negative or non-numeric limit
    std::string text;
};
[[nodiscard]] StyledText split_styled_text(const std::string& cell, const std::map<std::string, TextStyleRow>& styles);

/// Installer units -> pixels. Windows maps an installer unit to 1/12 of the
/// system text height (the TextHeight property: 16 px at 96 DPI) and
/// truncates (probe: 410x330 units -> 546x440 client px, 56x17 -> 74x22,
/// independent of DefaultUIFont).
struct UnitScale {
    i32 text_height{16};
    [[nodiscard]] i32 px(i32 units) const { return static_cast<i32>((static_cast<i64>(units) * text_height) / 12); }
};

struct PixelRect {
    i32 x{0}, y{0}, w{0}, h{0};
    [[nodiscard]] bool operator==(const PixelRect&) const = default;
};

// ---- Dialog model ----------------------------------------------------------------------

/// One Control-table (or BBControl-table) row.
struct ControlRow {
    std::string name;
    std::string type;
    i32 x{0}, y{0}, width{0}, height{0};
    u32 attributes{0};
    std::string property;
    std::string text;      ///< raw (Formatted, with style prefixes)
    std::string next;
    std::string help;
};

struct ControlEventRow {
    std::string event;     ///< raw (Formatted)
    std::string argument;  ///< raw (Formatted)
    std::string condition; ///< "" = blank
    std::optional<i32> ordering;
};

struct SubscriptionRow {
    std::string control;
    std::string event;
    std::string attribute;
};

/// Items of a RadioButtonGroup / ListBox / ComboBox / ListView.
struct ListItem {
    std::string value;
    std::string text;
    i32 order{0};
    i32 x{0}, y{0}, width{0}, height{0};  ///< RadioButton only
    std::string help;                     ///< RadioButton only
    std::string binary;                   ///< ListView icon (Binary key)
};

struct DialogRow {
    std::string name;
    i32 hcenter{50}, vcenter{50};
    i32 width{0}, height{0};
    u32 attributes{0};
    std::string title;
    std::string first, default_control, cancel;
};

/// A dialog with everything its controls need from the package, read once.
struct DialogModel {
    DialogRow row;
    std::vector<ControlRow> controls;                       ///< table order
    std::map<std::string, std::vector<ControlEventRow>> events;  ///< by control
    /// ControlCondition (control -> [(action, condition)]), table order.
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> conditions;
    std::vector<SubscriptionRow> subscriptions;
    [[nodiscard]] const ControlRow* control(const std::string& name) const;
    [[nodiscard]] bool is_error_dialog() const { return (row.attributes & sdk::msidbDialogAttributesError) != 0; }
    [[nodiscard]] bool is_modal() const { return (row.attributes & sdk::msidbDialogAttributesModal) != 0; }
};

/// Read a dialog from the package. nullopt when the Dialog table lacks it.
[[nodiscard]] std::optional<DialogModel> load_dialog(Package& package, const std::string& name);

/// Every text style in the TextStyle table.
[[nodiscard]] std::map<std::string, TextStyleRow> load_text_styles(Package& package);
/// UIText Key -> Text.
[[nodiscard]] std::map<std::string, std::string> load_ui_text(Package& package);
/// Rows of RadioButton / ListBox / ComboBox / ListView for @p property, in
/// Order order. @p table is the table name.
[[nodiscard]] std::vector<ListItem> load_list_items(Package& package, const std::string& table, const std::string& property);
/// BBControl rows of a billboard.
[[nodiscard]] std::vector<ControlRow> load_billboard_controls(Package& package, const std::string& billboard);

/// The events a control publishes when activated, after the ControlEvent
/// table's selection rules (Learn ControlEvent table): every row whose
/// condition is true, in Ordering order; when none is true, the one blank-
/// condition row with the largest Ordering; at most one NewDialog /
/// SpawnDialog (the selected one with the largest Ordering).
/// @p eval evaluates a condition (nullopt = syntax error, treated as false).
[[nodiscard]] std::vector<ControlEventRow> select_control_events(
    const std::vector<ControlEventRow>& rows, const std::function<std::optional<bool>(const std::string&)>& eval);

/// The installed-unit size text a FormatSize Text control shows for a value
/// in 512-byte units, with the package's UIText "bytes"/"KB"/"MB"/"GB"
/// strings. nullopt when the needed UIText key is missing.
[[nodiscard]] std::optional<std::string> format_size_text(i64 units_512, const std::map<std::string, std::string>& ui_text,
                                                          std::string* missing_key);

/// Segments of a MaskedEdit template's visible part (Learn MaskedEdit
/// control): runs of one editable kind, and literal text.
struct MaskSegment {
    enum class Kind : u8 { Literal, Digit, AltDigit, AnyChar, AltChar };
    Kind kind{Kind::Literal};
    std::string text;  ///< literal text, or the run's mask characters
    i32 length{0};     ///< editable positions
};
struct MaskTemplate {
    std::string hidden_prefix;   ///< text left of '<' (kept in the value)
    std::vector<MaskSegment> segments;
    std::string hidden_suffix;   ///< text right of '>'
};
[[nodiscard]] MaskTemplate parse_mask(const std::string& mask);

// ---- Window layout ---------------------------------------------------------------------

/// What one control window is created with (CreateWindowExW arguments).
struct WindowSpec {
    std::u16string class_name;
    std::u16string text;
    u32 style{0};
    u32 ex_style{0};
    PixelRect rect;   ///< in the parent's client coordinates
    u32 id{0};
    bool has_font{true};
    std::string control_type;  ///< the Control-table Type (input routing)
};

/// The window a control row is created as (class, style, ex-style, rect),
/// per the measured Windows layout (msi_ui_layout*.expected). @p origin_x/y
/// are the units the row's X/Y are relative to (a billboard's position for
/// BBControl rows: Windows converts the summed units, probe "Billboard
/// text"). nullopt for controls with no window of their own (Billboard).
[[nodiscard]] std::optional<WindowSpec> control_window_spec(const ControlRow& row, const UnitScale& scale,
                                                            bool is_default, i32 origin_x = 0, i32 origin_y = 0);
/// The dialog window: class, style, ex-style and the client size in pixels.
/// A dialog with a Control_Cancel has a close box ("MsiDialogCloseClass");
/// one without does not ("MsiDialogNoCloseClass", a CS_NOCLOSE class).
struct DialogWindowSpec {
    std::u16string class_name;
    bool no_close{false};
    std::u16string title;
    u32 style{0};
    u32 ex_style{0};
    i32 client_w{0}, client_h{0};
    i32 hcenter{50}, vcenter{50};
};
[[nodiscard]] DialogWindowSpec dialog_window_spec(const DialogRow& row, const UnitScale& scale);

// ---- Window system -----------------------------------------------------------------------

/// A user action or state change the dialog loop reacts to.
struct UiInput {
    enum class Kind : u8 {
        None,        ///< nothing yet (idle: SpawnWaitDialog conditions are re-checked)
        Activated,   ///< a push button / check box clicked, or Return on the default button
        Changed,     ///< an edit / combo / list / radio value changed
        Selected,    ///< a tree / list highlight moved (SelectionTree, DirectoryList)
        Close,       ///< Esc / the close box: the cancel control
    };
    Kind kind{Kind::None};
    u64 window{0};     ///< the control window
    std::string value; ///< Changed: the new text / selected item value / "1"/"" for a check box
};

/// The windows of the msiexec process. Implemented over user32 by the
/// runtime; the engine and view only call this.
class WindowSystem {
public:
    virtual ~WindowSystem() = default;
    /// LOGPIXELSY and the system text height the installer-unit scale uses.
    [[nodiscard]] virtual i32 dpi() const = 0;
    [[nodiscard]] virtual i32 system_text_height() const = 0;
    /// Pixel width and height of @p text in @p font (GetTextExtentPoint32W).
    [[nodiscard]] virtual std::pair<i32, i32> text_extent(const FontSpec& font, const std::u16string& text) = 0;
    /// Create the top-level dialog window (hidden). @p owner: the UI owner
    /// HWND or the parent dialog for a spawned child. 0 on failure.
    virtual u64 create_dialog(const DialogWindowSpec& spec, u64 owner) = 0;
    /// Create a child window of @p parent (a dialog or a group window). 0 on failure.
    virtual u64 create_control(u64 parent, const WindowSpec& spec) = 0;
    virtual void set_font(u64 window, const FontSpec& font, std::optional<u32> color) = 0;
    virtual void set_text(u64 window, const std::u16string& text) = 0;
    virtual void set_rich_text(u64 window, const std::string& rtf) = 0;
    virtual void set_visible(u64 window, bool visible) = 0;
    virtual void set_enabled(u64 window, bool enabled) = 0;
    virtual void set_checked(u64 window, bool checked) = 0;
    virtual void set_default_button(u64 dialog, u64 button) = 0;
    virtual void set_focus(u64 window) = 0;
    /// Image from Binary-table bytes (BMP / ICO); @p icon_size 0 = first image.
    virtual void set_image(u64 window, const std::vector<u8>& bytes, bool icon, i32 icon_size) = 0;
    /// A standard icon (IDI_ERROR / IDI_WARNING / IDI_INFORMATION).
    virtual void set_system_icon(u64 window, u32 idi) = 0;
    /// List / combo / list-view items (text, value), the selection by value.
    virtual void set_items(u64 window, const std::vector<std::pair<std::string, std::string>>& items,
                           const std::string& selected) = 0;
    /// Progress bar range and position.
    virtual void set_progress(u64 window, u32 position, u32 range) = 0;
    /// A SelectionTree's nodes. Each node carries the state menu the user
    /// picks from (UIText Menu* strings); a pick comes back as a Changed
    /// input with value "<key>\t<menu key>".
    struct TreeNode {
        std::string key;
        std::string parent;   ///< "" for a root
        std::string text;
        bool expanded{false};
        i32 state_image{0};   ///< index of the item's install-state image
        std::vector<std::pair<std::string, std::string>> menu;  ///< (menu key, text)
    };
    virtual void set_tree(u64 window, const std::vector<TreeNode>& nodes, const std::string& selected) = 0;
    virtual void move(u64 window, const PixelRect& rect) = 0;
    /// Position and show a top-level dialog (centering per H/VCentering).
    virtual void show_dialog(u64 dialog, i32 hcenter, i32 vcenter, bool activate) = 0;
    virtual void destroy(u64 window) = 0;
    /// Pump messages until the user does something on @p dialog (or one of
    /// its controls), or @p timeout_ms passes (Kind::None).
    virtual UiInput wait_input(u64 dialog, u32 timeout_ms) = 0;
    /// Pump pending messages without blocking (progress dialogs).
    virtual void pump() = 0;
    /// MessageBoxW (owner, text, caption, MB_* type) -> IDOK...
    virtual i32 message_box(u64 owner, const std::u16string& text, const std::u16string& caption, u32 type) = 0;
};

// ---- The internal UI -----------------------------------------------------------------------

/// What the dialogs need from the engine and the other areas. Defaults use
/// the session alone; the engine fills the rest.
struct UiHooks {
    /// MsiDoAction (DoAction ControlEvent, ValidateProductID).
    std::function<u32(Session&, const std::string&)> do_action;
    /// EnableRollback ControlEvent.
    std::function<void(Session&, bool)> enable_rollback;
    /// CheckTargetPath / CheckExistingTargetPath / SetTargetPath validity:
    /// true when @p path is a valid (and, with @p writable, writable) folder path.
    std::function<bool(Session&, const std::string& path, bool writable)> check_path;
    /// Folder names under a directory (DirectoryList) and volumes (combo
    /// controls): from the VFS.
    std::function<std::vector<std::string>(Session&, const std::string& path)> list_folders;
    std::function<bool(Session&, const std::string& path)> create_folder;
    std::function<std::vector<std::string>(Session&)> list_volumes;
    /// MsiLaunchApp ControlEvent.
    std::function<bool(Session&, const std::string& file, const std::string& args)> launch_app;
};

/// The UiHost the engine presents dialogs and progress with.
class InstallerUi : public UiHost {
public:
    /// @p level: a fixed INSTALLUILEVEL (tests, UI preview), or 0 to follow
    /// the engine: the level of the session's Engine (Engine::ui_level(),
    /// from the msiexec command line or MsiSetInternalUI) whenever a
    /// session is in hand, else MsiManager::engine_ui_level().
    InstallerUi(MsiManager& mgr, std::shared_ptr<WindowSystem> windows, UiHooks hooks, u32 level = 0);
    ~InstallerUi() override;
    [[nodiscard]] u32 level() const override;
    u32 run_dialog(Session& session, const std::string& dialog) override;
    i32 message(Session& session, u32 type, const Record& record) override;

    /// Test/inspection: the dialog currently on screen ("" when none).
    [[nodiscard]] std::string current_dialog() const;

private:
    friend struct InstallerUiPreviewAccess;
    struct Impl;
    /// The level for @p session (see the constructor).
    [[nodiscard]] u32 level_for(Session* session) const;
    std::unique_ptr<Impl> m_impl;
    MsiManager& m_mgr;
    u32 m_fixed_level;
};

// ---- The user32 window system ---------------------------------------------------------

/// What the user32-backed WindowSystem needs from the msiexec guest process:
/// calls of guest-visible exports (user32 / gdi32 / kernel32) by name,
/// guest scratch memory, and the guest address of the window procedure
/// export (kUiWindowProcExport) whose host side is ui_window_proc.
class UiGuestApi {
public:
    virtual ~UiGuestApi() = default;
    virtual u64 call(std::string_view dll, std::string_view function, const std::vector<u64>& args) = 0;
    /// Guest memory for one call's structures and strings (zeroed); 0 on failure.
    virtual GuestAddr alloc(u32 bytes) = 0;
    virtual void release(GuestAddr address) = 0;
    [[nodiscard]] virtual GuestMemory& memory() = 0;
    [[nodiscard]] virtual bool is_64bit() const = 0;
    [[nodiscard]] virtual GuestAddr window_proc() const = 0;
};

/// The internal export the dialog windows' class (and the group/container
/// windows it subclasses) use as WNDPROC. Its handler is ui_window_proc.
inline constexpr std::string_view kUiWindowProcExport = "VitreoMsiUiWindowProc";
/// `LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM)`: turns WM_COMMAND /
/// WM_NOTIFY / WM_CLOSE into UiInput for the window system that owns the
/// window, colours styled Text controls, and leaves the rest to DefDlgProcW
/// (dialogs) or the subclassed window's own procedure.
void ui_window_proc(Marshaler& m);

/// A WindowSystem that creates the dialogs with this process's user32
/// (CreateWindowExW of the classes control_window_spec names), measured
/// struct layouts: tools/oracle_probe/msi_ui_win32_layout*.expected.
[[nodiscard]] std::shared_ptr<WindowSystem> make_user32_window_system(std::shared_ptr<UiGuestApi> api);

/// UiHooks over this process's VFS (folder lists, volumes, path checks).
[[nodiscard]] UiHooks default_ui_hooks(MsiManager& mgr);

/// Give @p mgr an internal UI presented with @p windows: creates the
/// process's InstallerUi (following the engine's UI level), registers it with
/// MsiManager::set_ui_host() and set_ui_window_system(). A null @p windows
/// removes it (the engine then runs at basic UI, msi_sequence.cpp).
void install_internal_ui(MsiManager& mgr, std::shared_ptr<WindowSystem> windows);

// ---- Exports -----------------------------------------------------------------------------

/// MsiSetInternalUI's exchange (probe msi_ui_api "internal set" lines):
/// the modifier bits of @p requested always replace the stored ones; a base
/// level (the rest) of 0 keeps the stored base, 1..5 replaces it, anything
/// else leaves the base alone and returns INSTALLUILEVEL_NOCHANGE (and the
/// owner is not exchanged). Otherwise returns the previous full level.
/// Exposed for tests.
struct InternalUiChange {
    u32 returned{0};
    u32 new_level{0};
    bool valid{true};
};
[[nodiscard]] InternalUiChange apply_internal_ui_level(u32 stored, u32 requested);

}  // namespace vitreo::winapi::msi
