#pragma once

/// @file oledlg.hpp
/// @brief oledlg.dll — OLE common dialogs (oledlg.h).
///
/// OleUIInsertObjectW/A run a real modal Win32 dialog (a guest HWND, so the
/// hook, a custom template and the caller's owner all work as on Windows):
/// the template is Vitreo's own, laid out with the control ids oledlg.h
/// documents, and its dialog procedure is the internal export
/// VitreoOleUIInsertObjectDlgProc. Every window, resource, file and OLE
/// operation goes through the guest-visible user32 / kernel32 / comdlg32 /
/// ole32 exports, the way oledlg itself calls them. The other oledlg dialogs
/// fail with their documented result and a one-time UNIMPLEMENTED line.
///
/// Contract: Learn (OleUIInsertObject, OLEUIINSERTOBJECT), oledlg.h
/// 10.0.26100.0, and tools/oracle_probe/oledlg_insertobject_probe.c
/// (recorded in tests/oracle_data/oledlg_insertobject_probe{32,64}.expected)
/// for what Learn leaves out.
///
/// Known gaps (each logs `[vitreo] UNIMPLEMENTED oledlg.dll!...` once):
///  * IOF_CREATENEWOBJECT / CREATEFILEOBJECT / CREATELINKOBJECT call
///    ole32!OleCreate / OleCreateFromFile / OleCreateLinkToFile, and Display
///    As Icon calls ole32!OleGetIconOfClass / OleGetIconOfFile. Vitreo's
///    ole32 has none of them yet: creation then fails with sc = E_NOTIMPL
///    and OLEUI_IOERR_SCODEHASERROR, and hMetaPict is NULL.
///  * Change Icon (OleUIChangeIcon) and Add Control (control registration)
///    do nothing unless the hook handles OLEUI_MSG_CHANGEICON /
///    OLEUI_MSG_ADDCONTROL.
///  * Not measured (the Package object never returns under a non-interactive
///    winoracle session): what Windows does on a failed file or link
///    creation beyond the documented SCODEHASERROR; and whether a hook that
///    handles OLEUI_MSG_BROWSE has its buffer adopted (Vitreo adopts it).
///  * OleUIPasteSpecial, EditLinks, ChangeIcon, Convert, Busy,
///    ObjectProperties, ChangeSource: OLEUI_ERR_STRUCTURENULL for NULL, else
///    OLEUI_ERR_DIALOGFAILURE. OleUIAddVerbMenu, OleUICanConvertOrActivateAs
///    and OleUIUpdateLinks return FALSE; OleUIPromptUser returns -1.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <array>
#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::capsule {
class Registry;
}

namespace vitreo::winapi {

class Marshaler;

namespace oledlg {

// oledlg.h return codes.
inline constexpr u32 kOleUiOk = 1;
inline constexpr u32 kOleUiCancel = 2;
inline constexpr u32 kErrStructureNull = 101;
inline constexpr u32 kErrStructureInvalid = 102;
inline constexpr u32 kErrCbStructIncorrect = 103;
inline constexpr u32 kErrHwndOwnerInvalid = 104;
inline constexpr u32 kErrLpszTemplateInvalid = 108;
inline constexpr u32 kErrFindTemplateFailure = 110;
inline constexpr u32 kErrLoadTemplateFailure = 111;
inline constexpr u32 kErrDialogFailure = 112;
inline constexpr u32 kErrLocalMemAlloc = 113;
inline constexpr u32 kErrStandardMax = 116;
inline constexpr u32 kIoErrLpszFileInvalid = kErrStandardMax + 0;
inline constexpr u32 kIoErrScodeHasError = kErrStandardMax + 7;
inline constexpr u32 kIoErrLpClsidExcludeInvalid = kErrStandardMax + 8;
inline constexpr u32 kIoErrCchFileInvalid = kErrStandardMax + 9;

// IOF_* (oledlg.h).
inline constexpr u32 kIofShowHelp = 0x0001;
inline constexpr u32 kIofSelectCreateNew = 0x0002;
inline constexpr u32 kIofSelectCreateFromFile = 0x0004;
inline constexpr u32 kIofCheckLink = 0x0008;
inline constexpr u32 kIofCheckDisplayAsIcon = 0x0010;
inline constexpr u32 kIofCreateNewObject = 0x0020;
inline constexpr u32 kIofCreateFileObject = 0x0040;
inline constexpr u32 kIofCreateLinkObject = 0x0080;
inline constexpr u32 kIofDisableLink = 0x0100;
inline constexpr u32 kIofVerifyServersExist = 0x0200;
inline constexpr u32 kIofDisableDisplayAsIcon = 0x0400;
inline constexpr u32 kIofHideChangeIcon = 0x0800;
inline constexpr u32 kIofShowInsertControl = 0x1000;
inline constexpr u32 kIofSelectCreateControl = 0x2000;

// Control ids (oledlg.h).
inline constexpr u32 kIdcHelp = 99;
inline constexpr u32 kIdcCreateNew = 2100;
inline constexpr u32 kIdcCreateFromFile = 2101;
inline constexpr u32 kIdcLinkFile = 2102;
inline constexpr u32 kIdcObjectTypeList = 2103;
inline constexpr u32 kIdcDisplayAsIcon = 2104;
inline constexpr u32 kIdcChangeIcon = 2105;
inline constexpr u32 kIdcFile = 2106;
inline constexpr u32 kIdcFileDisplay = 2107;
inline constexpr u32 kIdcResultImage = 2108;
inline constexpr u32 kIdcResultText = 2109;
inline constexpr u32 kIdcIconDisplay = 2110;
inline constexpr u32 kIdcObjectTypeText = 2111;
inline constexpr u32 kIdcFileText = 2112;
inline constexpr u32 kIdcFileType = 2113;
inline constexpr u32 kIdcInsertControl = 2114;
inline constexpr u32 kIdcAddControl = 2115;
inline constexpr u32 kIdcControlTypeList = 2116;
inline constexpr u32 kIddInsertObject = 1000;
inline constexpr u32 kIdBrowseInsertFile = 2;
inline constexpr u32 kMaxPath = 260;

/// OLEUIINSERTOBJECTW/A field offsets (oledlg.h, pack 8). The A and W
/// structures share one layout: only the string types differ.
struct InsertObjectLayout {
    u32 size, flags, hwnd_owner, caption, hook, cust_data, hinstance, template_name,
        hresource, clsid, file, cch_file, c_clsid_exclude, clsid_exclude, iid,
        ole_render, format_etc, client_site, storage, ppv_obj, sc, hmeta_pict;
};
inline constexpr InsertObjectLayout kInsertObject32{112, 4, 8, 12, 16, 20, 24, 28, 32, 36, 52, 56,
                                                    60, 64, 68, 84, 88, 92, 96, 100, 104, 108};
inline constexpr InsertObjectLayout kInsertObject64{176, 4, 8, 16, 24, 32, 40, 48, 56, 64, 80, 88,
                                                    92, 96, 104, 120, 128, 136, 144, 152, 160, 168};

/// One export of oledlg.dll: name, ordinal (the DLL's export table; the SDK
/// import library links the A functions and OleUICanConvertOrActivateAs by
/// these ordinals) and x86 stdcall argument bytes (OleDlg.lib decoration;
/// OleUIPromptUser is cdecl).
struct ExportSpec {
    const char* name;
    u16 ordinal;
    u16 stdcall_bytes;
};
inline constexpr std::array<ExportSpec, 23> kExports{{
    {"OleUIAddVerbMenuA", 1, 36},     {"OleUICanConvertOrActivateAs", 2, 12},
    {"OleUIInsertObjectA", 3, 4},     {"OleUIPasteSpecialA", 4, 4},
    {"OleUIEditLinksA", 5, 4},        {"OleUIChangeIconA", 6, 4},
    {"OleUIConvertA", 7, 4},          {"OleUIBusyA", 8, 4},
    {"OleUIUpdateLinksA", 9, 16},     {"OleUIPromptUserA", 10, 0},
    {"OleUIObjectPropertiesA", 11, 4}, {"OleUIChangeSourceA", 12, 4},
    {"OleUIAddVerbMenuW", 13, 36},    {"OleUIBusyW", 14, 4},
    {"OleUIChangeIconW", 15, 4},      {"OleUIChangeSourceW", 16, 4},
    {"OleUIConvertW", 17, 4},         {"OleUIEditLinksW", 18, 4},
    {"OleUIInsertObjectW", 19, 4},    {"OleUIObjectPropertiesW", 20, 4},
    {"OleUIPasteSpecialW", 21, 4},    {"OleUIPromptUserW", 22, 0},
    {"OleUIUpdateLinksW", 23, 16},
}};

/// One entry of the Insert Object list: "<name>\t{CLSID}" is its text.
struct ListEntry {
    std::string name;  ///< UTF-8
    std::array<u8, 16> clsid{};
};

/// "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" (StringFromGUID2 form) for a
/// GUID in its in-memory byte order, and the reverse.
[[nodiscard]] std::string guid_string(const std::array<u8, 16>& guid);
[[nodiscard]] std::optional<std::array<u8, 16>> parse_guid(std::string_view text);

} // namespace oledlg

class OledlgManager {
public:
    /// Invokes a guest function (an export's thunk, the hook) with
    /// native-width arguments; mirrors ComCoreManager::GuestCall.
    using GuestCall = std::function<u64(GuestAddr, const u64*, usize, bool)>;

    void set_guest_call(GuestCall call) { guest_call_ = std::move(call); }
    void set_thunk_registry(ThunkRegistry* thunks) noexcept { thunks_ = thunks; }
    void set_registry(capsule::Registry* registry) noexcept { registry_ = registry; }

    void impl_OleUIInsertObject(Marshaler& m, bool wide);
    /// The dialog procedure of the Insert Object dialog
    /// (internal export VitreoOleUIInsertObjectDlgProc).
    void impl_InsertObjectDlgProc(Marshaler& m);

    /// The Insertable classes HKCR registers, exactly as the dialog lists
    /// them (sorted, de-duplicated, minus @p exclude), for @p verify servers.
    [[nodiscard]] std::vector<oledlg::ListEntry> insertable_classes(
        Marshaler& m, const std::vector<std::array<u8, 16>>& exclude, bool verify);
    /// The ActiveX controls (HKCR\CLSID\*\Control) for the Insert Control mode.
    [[nodiscard]] std::vector<oledlg::ListEntry> control_classes(
        const std::vector<std::array<u8, 16>>& exclude);

private:
    enum class Mode : u8 { None, New, File, Control };
    struct Dialog {
        bool wide{true};
        GuestAddr io{0};
        u32 flags{0};
        u32 owner{0};
        GuestAddr hook{0};
        u64 cust_data{0};
        GuestAddr file{0};
        u32 cch_file{0};
        std::vector<std::array<u8, 16>> exclude;
        Mode mode{Mode::None};
        u32 hwnd{0};
        GuestAddr scratch{0};        ///< guest LocalAlloc block: template, then strings
        u32 scratch_used{0};
        u32 msg_enddialog{0}, msg_browse{0}, msg_browse_ofn{0}, msg_help{0},
            msg_changeicon{0}, msg_addcontrol{0};
        u32 result{0};
    };

    u64 call(Marshaler& m, const char* dll, const char* name, std::initializer_list<u64> args,
             bool* missing = nullptr);
    GuestAddr scratch_string(Marshaler& m, Dialog& d, std::string_view utf8);
    std::string dlg_item_text(Marshaler& m, Dialog& d, u32 id);
    u32 item(Marshaler& m, Dialog& d, u32 id);
    void show(Marshaler& m, Dialog& d, u32 id, bool visible);
    void enable(Marshaler& m, Dialog& d, u32 id, bool enabled);
    bool checked(Marshaler& m, Dialog& d, u32 id);
    void set_check(Marshaler& m, Dialog& d, u32 id, bool on);
    void fill_list(Marshaler& m, Dialog& d, const std::vector<oledlg::ListEntry>& entries);
    void set_mode(Marshaler& m, Dialog& d, Mode mode);
    void update_icon_controls(Marshaler& m, Dialog& d);
    void update_result(Marshaler& m, Dialog& d);
    std::optional<std::array<u8, 16>> selected_clsid(Marshaler& m, Dialog& d);
    u64 call_hook(Marshaler& m, Dialog& d, u32 msg, u64 wp, u64 lp);
    void end_dialog(Marshaler& m, Dialog& d, u32 result);
    void browse(Marshaler& m, Dialog& d);
    void on_init(Marshaler& m, Dialog& d);
    void on_command(Marshaler& m, Dialog& d, u32 id, u32 code);
    u32 finish_ok(Marshaler& m, Dialog& d);
    bool server_exists(Marshaler& m, const std::string& command);
    Dialog* find_dialog(u32 hwnd);

    GuestCall guest_call_;
    ThunkRegistry* thunks_{nullptr};
    capsule::Registry* registry_{nullptr};
    std::mutex mutex_;
    std::map<u64, Dialog*> pending_;     ///< by cookie, until WM_INITDIALOG
    std::map<u32, Dialog*> dialogs_;     ///< by HWND, while the dialog runs
    u64 next_cookie_{0x4F4C4501};
};

[[nodiscard]] DllExports create_oledlg_thunks(OledlgManager& mgr);

} // namespace vitreo::winapi
