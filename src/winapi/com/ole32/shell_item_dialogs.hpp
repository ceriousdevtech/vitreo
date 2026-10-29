#pragma once
/// @file shell_item_dialogs.hpp
/// @brief The Common Item Dialog (CLSID_FileOpenDialog / CLSID_FileSaveDialog)
///        and the shell items it hands out (IShellItem, IShellItemArray,
///        SHCreateItemFromParsingName), presented as NSOpenPanel /
///        NSSavePanel.
///
/// Contract: Microsoft Learn (shobjidl_core.h) for the interfaces; vtable
/// order, IIDs and FOS_/SIGDN_/SFGAO_ values from the SDK header; defaults,
/// ordering errors and display names measured by
/// tools/oracle_probe/file_dialog_probe{32,64}.expected (x86 = x64).

#include <vitreo/common.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::winapi {

class ComCoreManager;
class Marshaler;
struct DllExports;
struct Guid;

class ShellItemDialogs {
public:
    explicit ShellItemDialogs(ComCoreManager& com) noexcept : m_com(com) {}

    [[nodiscard]] static bool is_dialog_class(const Guid& clsid, bool& save);

    /// CoCreateInstance for CLSID_FileOpenDialog / CLSID_FileSaveDialog.
    [[nodiscard]] u32 create_dialog(Marshaler& m, bool save, const Guid& iid,
                                    GuestAddr ppv);
    /// HRESULT SHCreateItemFromParsingName(PCWSTR, IBindCtx*, REFIID, void**).
    [[nodiscard]] u32 create_item_from_parsing_name(Marshaler& m);

    /// The Vitreo_FileDialog_* / Vitreo_ShellItem_* / Vitreo_ShellItemArray_*
    /// method thunks (ole32.dll).
    void append_thunks(DllExports& exports);

private:
    struct Sink {
        GuestAddr events = 0;
        u32 cookie = 0;
    };
    struct Dialog {
        bool save = false;
        u32 refs = 1;
        u32 options = 0;
        bool types_set = false;
        std::vector<std::pair<std::string, std::string>> types;  ///< name, spec
        u32 type_index = 0;
        std::optional<std::string> file_name;
        std::string title, ok_label, file_name_label, default_extension;
        std::string folder, default_folder;   ///< guest paths
        std::vector<Sink> sinks;
        u32 next_cookie = 1;
        bool have_result = false;
        std::vector<std::string> results;     ///< guest paths
        bool showing = false;
    };
    struct Item {
        std::string path;   ///< canonical guest path; kThisPc for the virtual folder
        u32 refs = 1;
    };
    struct ItemArray {
        std::vector<std::string> paths;
        u32 refs = 1;
    };

    // Dialog methods (IFileDialog slots 0..26, then open/save extensions).
    u32 dialog_query_interface(Marshaler& m);
    u32 dialog_add_ref(Marshaler& m);
    u32 dialog_release(Marshaler& m);
    u32 dialog_show(Marshaler& m);
    u32 dialog_set_file_types(Marshaler& m);
    u32 dialog_set_file_type_index(Marshaler& m);
    u32 dialog_get_file_type_index(Marshaler& m);
    u32 dialog_advise(Marshaler& m);
    u32 dialog_unadvise(Marshaler& m);
    u32 dialog_set_options(Marshaler& m);
    u32 dialog_get_options(Marshaler& m);
    u32 dialog_set_folder(Marshaler& m, bool default_folder);
    u32 dialog_get_folder(Marshaler& m);
    u32 dialog_get_current_selection(Marshaler& m);
    u32 dialog_set_string(Marshaler& m, std::string Dialog::* field);
    u32 dialog_set_file_name(Marshaler& m);
    u32 dialog_get_file_name(Marshaler& m);
    u32 dialog_get_result(Marshaler& m);
    u32 dialog_add_place(Marshaler& m);
    u32 dialog_close(Marshaler& m);
    u32 dialog_set_client_guid(Marshaler& m);
    u32 dialog_clear_client_data(Marshaler& m);
    u32 dialog_set_filter(Marshaler& m);
    u32 dialog_get_results(Marshaler& m);
    u32 dialog_get_selected_items(Marshaler& m);
    u32 dialog_set_save_as_item(Marshaler& m);
    u32 dialog_property_store(Marshaler& m, const char* method);

    // IShellItem methods.
    u32 item_query_interface(Marshaler& m);
    u32 item_add_ref(Marshaler& m);
    u32 item_release(Marshaler& m);
    u32 item_bind_to_handler(Marshaler& m);
    u32 item_get_parent(Marshaler& m);
    u32 item_get_display_name(Marshaler& m);
    u32 item_get_attributes(Marshaler& m);
    u32 item_compare(Marshaler& m);

    // IShellItemArray methods.
    u32 array_query_interface(Marshaler& m);
    u32 array_add_ref(Marshaler& m);
    u32 array_release(Marshaler& m);
    u32 array_unsupported(Marshaler& m, const char* method);
    u32 array_get_attributes(Marshaler& m);
    u32 array_get_count(Marshaler& m);
    u32 array_get_item_at(Marshaler& m);

    /// A new IShellItem for @p path (caller owns the one reference); 0 when
    /// the COM arena is exhausted.
    GuestAddr new_item(Marshaler& m, const std::string& path);
    GuestAddr new_array(Marshaler& m, std::vector<std::string> paths);
    /// Canonical guest path of an IShellItem the guest passed in: ours by
    /// lookup, a foreign one through its GetDisplayName(SIGDN_FILESYSPATH).
    std::optional<std::string> item_path(Marshaler& m, GuestAddr item);
    /// Existing path in canonical case, or nullopt (ERROR_FILE_NOT_FOUND).
    std::optional<std::string> canonical_path(const std::string& guest) const;
    std::optional<std::string> host_path(const std::string& guest) const;
    std::optional<std::string> guest_path(const std::string& host) const;
    u32 attributes_of(const std::string& path) const;
    std::string display_name(const std::string& path, u32 sigdn, u32& hr) const;
    u32 write_task_string(Marshaler& m, GuestAddr out, const std::string& text);

    ComCoreManager& m_com;
    std::mutex m_mutex;
    std::map<GuestAddr, std::shared_ptr<Dialog>> m_dialogs;
    std::map<GuestAddr, Item> m_items;
    std::map<GuestAddr, ItemArray> m_arrays;
    /// Released objects, reused so the fixed COM arena is not exhausted.
    std::vector<GuestAddr> m_free_open_dialogs, m_free_save_dialogs,
        m_free_items, m_free_arrays;
};

} // namespace vitreo::winapi
