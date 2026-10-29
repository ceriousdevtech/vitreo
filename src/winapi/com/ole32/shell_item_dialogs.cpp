/// @file shell_item_dialogs.cpp
/// @brief Common Item Dialog, IShellItem and IShellItemArray (see header).

#include "shell_item_dialogs.hpp"

#include "com_core_internal.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/ui/comdlg32/comdlg32.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vitreo::winapi {

namespace {

// shobjidl_core.h (Windows SDK 10.0.26100).
constexpr Guid CLSID_FILEOPENDIALOG{0xDC1C5A9Cu, 0xE88Au, 0x4DDEu, {0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7}};
constexpr Guid CLSID_FILESAVEDIALOG{0xC0B4E2F3u, 0xBA21u, 0x4773u, {0x8D, 0xBA, 0x33, 0x5E, 0xC9, 0x46, 0xEB, 0x8B}};
constexpr Guid IID_IMODALWINDOW{0xB4DB1657u, 0x70D7u, 0x485Eu, {0x8E, 0x3E, 0x6F, 0xCB, 0x5A, 0x5C, 0x18, 0x02}};
constexpr Guid IID_IFILEDIALOG{0x42F85136u, 0xDB7Eu, 0x439Cu, {0x85, 0xF1, 0xE4, 0x07, 0x5D, 0x13, 0x5F, 0xC8}};
constexpr Guid IID_IFILEOPENDIALOG{0xD57C7288u, 0xD4ADu, 0x4768u, {0xBE, 0x02, 0x9D, 0x96, 0x95, 0x32, 0xD9, 0x60}};
constexpr Guid IID_IFILESAVEDIALOG{0x84BCCD23u, 0x5FDEu, 0x4CDBu, {0xAE, 0xA4, 0xAF, 0x64, 0xB8, 0x3D, 0x78, 0xAB}};
constexpr Guid IID_IFILEDIALOGCUSTOMIZE{0xE6FDD21Au, 0x163Fu, 0x4975u, {0x9C, 0x8C, 0xA6, 0x9F, 0x1B, 0xA3, 0x70, 0x34}};
constexpr Guid IID_IFILEDIALOG2{0x61744FC7u, 0x85B5u, 0x4791u, {0xA9, 0xB0, 0x27, 0x22, 0x76, 0x30, 0x9B, 0x13}};
constexpr Guid IID_IOBJECTWITHSITE{0xFC4801A3u, 0x2BA9u, 0x11CFu, {0xA2, 0x29, 0x00, 0xAA, 0x00, 0x3D, 0x73, 0x52}};
constexpr Guid IID_IOLEWINDOW{0x00000114u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr Guid IID_ISHELLITEM{0x43826D1Eu, 0xE718u, 0x42EEu, {0xBC, 0x55, 0xA1, 0xE2, 0x61, 0xC3, 0x7B, 0xFE}};
constexpr Guid IID_ISHELLITEM2{0x7E9FB0D3u, 0x919Fu, 0x4307u, {0xAB, 0x2E, 0x9B, 0x18, 0x60, 0x31, 0x0C, 0x93}};
constexpr Guid IID_ISHELLITEMARRAY{0xB63EA76Du, 0x1F85u, 0x456Fu, {0xA1, 0x9C, 0x48, 0x15, 0x9E, 0xFA, 0x85, 0x8B}};

constexpr u32 S_OK = 0, S_FALSE = 1;
constexpr u32 E_UNEXPECTED = 0x8000FFFFu;
constexpr u32 HRESULT_CANCELLED = 0x800704C7u;        // HRESULT_FROM_WIN32(ERROR_CANCELLED)
constexpr u32 HRESULT_FILE_NOT_FOUND = 0x80070002u;  // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
constexpr u32 MK_E_NOOBJECT = 0x800401E5u;

// FILEOPENDIALOGOPTIONS (shobjidl_core.h).
constexpr u32 FOS_OVERWRITEPROMPT = 0x2, FOS_NOCHANGEDIR = 0x8, FOS_PICKFOLDERS = 0x20,
              FOS_ALLOWMULTISELECT = 0x200, FOS_PATHMUSTEXIST = 0x800,
              FOS_FILEMUSTEXIST = 0x1000, FOS_NOREADONLYRETURN = 0x8000,
              FOS_FORCESHOWHIDDEN = 0x10000000;
// Defaults measured by file_dialog_probe: open 0x1808, save 0x880A.
constexpr u32 kOpenDefaultOptions = FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR;
constexpr u32 kSaveDefaultOptions =
    FOS_OVERWRITEPROMPT | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST | FOS_NOREADONLYRETURN;

// SIGDN (shobjidl_core.h).
constexpr u32 SIGDN_NORMALDISPLAY = 0, SIGDN_PARENTRELATIVEPARSING = 0x80018001u,
              SIGDN_DESKTOPABSOLUTEPARSING = 0x80028000u,
              SIGDN_PARENTRELATIVEEDITING = 0x80031001u,
              SIGDN_DESKTOPABSOLUTEEDITING = 0x8004C000u, SIGDN_FILESYSPATH = 0x80058000u,
              SIGDN_URL = 0x80068000u, SIGDN_PARENTRELATIVEFORADDRESSBAR = 0x8007C001u;

// SFGAO (shobjidl_core.h).
constexpr u32 SFGAO_LINK = 0x00010000u, SFGAO_READONLY = 0x00040000u,
              SFGAO_HIDDEN = 0x00080000u, SFGAO_STREAM = 0x00400000u,
              SFGAO_FILESYSANCESTOR = 0x10000000u, SFGAO_FOLDER = 0x20000000u,
              SFGAO_FILESYSTEM = 0x40000000u, SFGAO_HASSUBFOLDER = 0x80000000u;
constexpr u32 SIATTRIBFLAGS_AND = 1, SIATTRIBFLAGS_OR = 2, SIATTRIBFLAGS_APPCOMPAT = 3;

/// The virtual "This PC" folder: the parent of drive roots and the item an
/// empty parsing name produces (file_dialog_probe "empty").
constexpr std::string_view kThisPc = "::{20D04FE0-3AEA-1069-A2D8-08002B30309D}";

void unimplemented_once(const char* what) {
    static std::mutex mutex;
    static std::vector<std::string> logged;
    std::lock_guard lock(mutex);
    if (std::find(logged.begin(), logged.end(), what) != logged.end()) return;
    logged.emplace_back(what);
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED %s\n", what);
}

bool is_root(const std::string& path) {
    return path.size() == 3 && path[1] == ':' && path[2] == '\\';
}

std::string leaf_of(const std::string& path) {
    if (is_root(path)) return path.substr(0, 2);
    const auto slash = path.find_last_of('\\');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string parent_of(const std::string& path) {
    const auto slash = path.find_last_of('\\');
    if (slash == std::string::npos) return {};
    return slash == 2 ? path.substr(0, 3) : path.substr(0, slash);
}

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string file_url(const std::string& path) {
    std::string url = "file:///";
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (unsigned char c : path) {
        if (c == '\\') {
            url.push_back('/');
        } else if (c == ' ' || c == '%' || c == '#' || c >= 0x80) {
            url.push_back('%');
            url.push_back(kHex[c >> 4]);
            url.push_back(kHex[c & 15]);
        } else {
            url.push_back(static_cast<char>(c));
        }
    }
    return url;
}

/// "*.txt;*.md" -> {"txt", "md"}; "*.*" or "*" -> {"*"}.
std::vector<std::string> spec_extensions(const std::string& spec) {
    std::vector<std::string> out;
    usize start = 0;
    while (start <= spec.size()) {
        const usize end = std::min(spec.find(';', start), spec.size());
        std::string pattern = spec.substr(start, end - start);
        pattern.erase(0, pattern.find_first_not_of(' '));
        if (pattern == "*.*" || pattern == "*") {
            out.emplace_back("*");
        } else if (pattern.rfind("*.", 0) == 0 && pattern.size() > 2) {
            out.push_back(pattern.substr(2));
        }
        start = end + 1;
    }
    return out;
}

}  // namespace

ShellItemDialogs& ComCoreManager::item_dialogs() {
    std::call_once(m_item_dialogs_once,
                   [this] { m_item_dialogs = std::make_shared<ShellItemDialogs>(*this); });
    return *m_item_dialogs;
}

bool ShellItemDialogs::is_dialog_class(const Guid& clsid, bool& save) {
    if (guid_equal(clsid, CLSID_FILEOPENDIALOG)) { save = false; return true; }
    if (guid_equal(clsid, CLSID_FILESAVEDIALOG)) { save = true; return true; }
    return false;
}

// -----------------------------------------------------------------------------
// Paths
// -----------------------------------------------------------------------------

std::optional<std::string> ShellItemDialogs::host_path(const std::string& guest) const {
    capsule::Vfs* vfs = m_com.vfs();
    if (vfs == nullptr) return std::nullopt;
    auto resolved = vfs->resolve_host_path(guest);
    if (resolved.is_err()) return std::nullopt;
    return resolved.value();
}

std::optional<std::string> ShellItemDialogs::guest_path(const std::string& host) const {
    auto guest = guest_path_for_host(m_com.vfs(), host);
    if (!guest) return std::nullopt;
    if (!guest->empty()) (*guest)[0] = static_cast<char>(std::toupper(static_cast<unsigned char>((*guest)[0])));
    if (guest->size() > 3 && guest->back() == '\\') guest->pop_back();
    return guest;
}

std::optional<std::string> ShellItemDialogs::canonical_path(const std::string& input) const {
    std::string path = input;
    std::replace(path.begin(), path.end(), '/', '\\');
    if (path.size() < 2 || path[1] != ':' ||
        !std::isalpha(static_cast<unsigned char>(path[0]))) {
        return std::nullopt;   // relative names do not parse (file_dialog_probe)
    }
    if (path.size() == 2) path.push_back('\\');
    while (path.size() > 3 && path.back() == '\\') path.pop_back();
    const auto host = host_path(path);
    if (!host) return std::nullopt;
    struct stat st{};
    if (::stat(host->c_str(), &st) != 0) return std::nullopt;
    // The volume's own spelling: "c:\windows\NOTEPAD.EXE" names
    // "C:\Windows\notepad.exe" (file_dialog_probe "lowercase"). Each component
    // takes the case of the directory entry it matches, walked from the drive
    // root so the result stays inside the drive even when its host root is
    // reached through a symlink.
    path[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(path[0])));
    std::string canonical = path.substr(0, 3);
    auto dir_host = host_path(canonical);
    usize start = 3;
    while (dir_host && start < path.size()) {
        const usize end = std::min(path.find('\\', start), path.size());
        const std::string component = path.substr(start, end - start);
        std::string spelled = component;
        if (DIR* dir = ::opendir(dir_host->c_str())) {
            while (dirent* entry = ::readdir(dir)) {
                if (::strcasecmp(entry->d_name, component.c_str()) == 0) {
                    spelled = entry->d_name;
                    break;
                }
            }
            ::closedir(dir);
        }
        if (canonical.size() > 3) canonical.push_back('\\');
        canonical += spelled;
        *dir_host += "/" + spelled;
        start = end + 1;
    }
    return canonical;
}

std::optional<std::string> ShellItemDialogs::item_path(Marshaler& m, GuestAddr item) {
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_items.find(item);
        if (found != m_items.end()) return found->second.path;
    }
    if (item == 0) return std::nullopt;
    // A guest-implemented IShellItem: ask it, as the shell would.
    const GuestAddr out = m_com.co_task_mem_alloc_guest(8);
    if (out == 0) return std::nullopt;
    (void)m.write_ptr(out, 0);
    const u64 args[3] = {item, SIGDN_FILESYSPATH, out};
    const u32 hr = m_com.call_guest_slot(m, item, 5, args, 3);
    std::optional<std::string> path;
    const GuestAddr text = m.read_ptr(out).value_or(0);
    if (hr == S_OK && text != 0) {
        auto read = m.read_wide_string(text);
        if (read.is_ok()) path = canonical_path(read.value());
        m_com.co_task_mem_free_guest(text);
    }
    m_com.co_task_mem_free_guest(out);
    return path;
}

u32 ShellItemDialogs::write_task_string(Marshaler& m, GuestAddr out, const std::string& text) {
    // UTF-8 bytes bound the UTF-16 units.
    const GuestAddr buffer = m_com.co_task_mem_alloc_guest((text.size() + 1) * 2);
    if (buffer == 0) {
        (void)m.write_ptr(out, 0);
        return E_OUTOFMEMORY;
    }
    if (m.write_wide_string(buffer, text).is_err() || m.write_ptr(out, buffer).is_err()) {
        m_com.co_task_mem_free_guest(buffer);
        return com::E_INVALIDARG;
    }
    return S_OK;
}

u32 ShellItemDialogs::attributes_of(const std::string& path) const {
    if (path == kThisPc) {
        return SFGAO_HASSUBFOLDER | SFGAO_FOLDER | SFGAO_FILESYSANCESTOR;
    }
    const auto host = host_path(path);
    struct stat st{};
    if (!host || ::stat(host->c_str(), &st) != 0) return 0;
    u32 attributes = SFGAO_FILESYSTEM;
    if (S_ISDIR(st.st_mode)) {
        attributes |= SFGAO_FOLDER | SFGAO_FILESYSANCESTOR;
        if (DIR* dir = ::opendir(host->c_str())) {
            while (dirent* entry = ::readdir(dir)) {
                if (entry->d_type == DT_DIR && std::strcmp(entry->d_name, ".") != 0 &&
                    std::strcmp(entry->d_name, "..") != 0) {
                    attributes |= SFGAO_HASSUBFOLDER;
                    break;
                }
            }
            ::closedir(dir);
        }
    } else {
        attributes |= SFGAO_STREAM;
        if (lower(leaf_of(path)).ends_with(".lnk")) attributes |= SFGAO_LINK;
    }
    if ((st.st_mode & S_IWUSR) == 0) attributes |= SFGAO_READONLY;
    if ((st.st_flags & UF_HIDDEN) != 0) attributes |= SFGAO_HIDDEN;
    return attributes;
}

std::string ShellItemDialogs::display_name(const std::string& path, u32 sigdn, u32& hr) const {
    hr = S_OK;
    const bool absolute = sigdn == SIGDN_DESKTOPABSOLUTEPARSING ||
                          sigdn == SIGDN_DESKTOPABSOLUTEEDITING || sigdn == SIGDN_FILESYSPATH;
    if (path == kThisPc) {
        if (sigdn == SIGDN_FILESYSPATH) { hr = com::E_INVALIDARG; return {}; }
        if (sigdn == SIGDN_URL) { hr = com::E_NOTIMPL; return {}; }
        if (sigdn == SIGDN_PARENTRELATIVEPARSING || sigdn == SIGDN_DESKTOPABSOLUTEPARSING) {
            return std::string(kThisPc);
        }
        return "This PC";
    }
    if (sigdn == SIGDN_URL) return file_url(path);
    if (absolute) return path;
    if (!is_root(path)) return leaf_of(path);
    // A drive root (file_dialog_probe "root"): "Local Disk (C:)", "C:" ...
    std::string label;
    if (capsule::Vfs* vfs = m_com.vfs()) {
        capsule::VolumeInfo volume{};
        auto resolved = vfs->resolve_components(path);
        if (resolved.is_ok() && resolved.value().first->volume_info(volume)) label = volume.label;
    }
    if (label.empty()) label = "Local Disk";
    const std::string drive = path.substr(0, 2);
    if (sigdn == SIGDN_PARENTRELATIVEPARSING || sigdn == SIGDN_PARENTRELATIVEFORADDRESSBAR) {
        return drive;
    }
    if (sigdn == SIGDN_PARENTRELATIVEEDITING) return label;
    return label + " (" + drive + ")";
}

// -----------------------------------------------------------------------------
// Object allocation
// -----------------------------------------------------------------------------

namespace {
constexpr std::string_view kDialogCommon[] = {
    "Vitreo_FileDialog_QueryInterface", "Vitreo_FileDialog_AddRef",
    "Vitreo_FileDialog_Release", "Vitreo_FileDialog_Show",
    "Vitreo_FileDialog_SetFileTypes", "Vitreo_FileDialog_SetFileTypeIndex",
    "Vitreo_FileDialog_GetFileTypeIndex", "Vitreo_FileDialog_Advise",
    "Vitreo_FileDialog_Unadvise", "Vitreo_FileDialog_SetOptions",
    "Vitreo_FileDialog_GetOptions", "Vitreo_FileDialog_SetDefaultFolder",
    "Vitreo_FileDialog_SetFolder", "Vitreo_FileDialog_GetFolder",
    "Vitreo_FileDialog_GetCurrentSelection", "Vitreo_FileDialog_SetFileName",
    "Vitreo_FileDialog_GetFileName", "Vitreo_FileDialog_SetTitle",
    "Vitreo_FileDialog_SetOkButtonLabel", "Vitreo_FileDialog_SetFileNameLabel",
    "Vitreo_FileDialog_GetResult", "Vitreo_FileDialog_AddPlace",
    "Vitreo_FileDialog_SetDefaultExtension", "Vitreo_FileDialog_Close",
    "Vitreo_FileDialog_SetClientGuid", "Vitreo_FileDialog_ClearClientData",
    "Vitreo_FileDialog_SetFilter",
};
constexpr std::string_view kOpenExtra[] = {
    "Vitreo_FileDialog_GetResults", "Vitreo_FileDialog_GetSelectedItems",
};
constexpr std::string_view kSaveExtra[] = {
    "Vitreo_FileDialog_SetSaveAsItem", "Vitreo_FileDialog_SetProperties",
    "Vitreo_FileDialog_SetCollectedProperties", "Vitreo_FileDialog_GetProperties",
    "Vitreo_FileDialog_ApplyProperties",
};
constexpr std::string_view kItemMethods[] = {
    "Vitreo_ShellItem_QueryInterface", "Vitreo_ShellItem_AddRef",
    "Vitreo_ShellItem_Release", "Vitreo_ShellItem_BindToHandler",
    "Vitreo_ShellItem_GetParent", "Vitreo_ShellItem_GetDisplayName",
    "Vitreo_ShellItem_GetAttributes", "Vitreo_ShellItem_Compare",
};
constexpr std::string_view kArrayMethods[] = {
    "Vitreo_ShellItemArray_QueryInterface", "Vitreo_ShellItemArray_AddRef",
    "Vitreo_ShellItemArray_Release", "Vitreo_ShellItemArray_BindToHandler",
    "Vitreo_ShellItemArray_GetPropertyStore",
    "Vitreo_ShellItemArray_GetPropertyDescriptionList",
    "Vitreo_ShellItemArray_GetAttributes", "Vitreo_ShellItemArray_GetCount",
    "Vitreo_ShellItemArray_GetItemAt", "Vitreo_ShellItemArray_EnumItems",
};
}  // namespace

u32 ShellItemDialogs::create_dialog(Marshaler& m, bool save, const Guid& iid, GuestAddr ppv) {
    const bool supported =
        guid_equal(iid, IID_IUNKNOWN) || guid_equal(iid, IID_IMODALWINDOW) ||
        guid_equal(iid, IID_IFILEDIALOG) ||
        guid_equal(iid, save ? IID_IFILESAVEDIALOG : IID_IFILEOPENDIALOG);
    if (!supported) {
        (void)m.write_ptr(ppv, 0);
        return com::E_NOINTERFACE;
    }
    GuestAddr object = 0;
    {
        std::lock_guard lock(m_mutex);
        auto& free_list = save ? m_free_save_dialogs : m_free_open_dialogs;
        if (!free_list.empty()) {
            object = free_list.back();
            free_list.pop_back();
        }
    }
    if (object == 0) {
        std::vector<std::string_view> methods(std::begin(kDialogCommon), std::end(kDialogCommon));
        if (save) methods.insert(methods.end(), std::begin(kSaveExtra), std::end(kSaveExtra));
        else methods.insert(methods.end(), std::begin(kOpenExtra), std::end(kOpenExtra));
        object = m_com.allocate_builtin_object(m, methods);
        if (object == 0) return E_OUTOFMEMORY;
    }
    auto dialog = std::make_shared<Dialog>();
    dialog->save = save;
    dialog->options = save ? kSaveDefaultOptions : kOpenDefaultOptions;
    {
        std::lock_guard lock(m_mutex);
        m_dialogs[object] = dialog;
    }
    if (m.write_ptr(ppv, object).is_err()) {
        std::lock_guard lock(m_mutex);
        m_dialogs.erase(object);
        (save ? m_free_save_dialogs : m_free_open_dialogs).push_back(object);
        return com::E_INVALIDARG;
    }
    return S_OK;
}

GuestAddr ShellItemDialogs::new_item(Marshaler& m, const std::string& path) {
    GuestAddr object = 0;
    {
        std::lock_guard lock(m_mutex);
        if (!m_free_items.empty()) {
            object = m_free_items.back();
            m_free_items.pop_back();
        }
    }
    if (object == 0) object = m_com.allocate_builtin_object(m, kItemMethods);
    if (object == 0) return 0;
    std::lock_guard lock(m_mutex);
    m_items[object] = Item{path, 1};
    return object;
}

GuestAddr ShellItemDialogs::new_array(Marshaler& m, std::vector<std::string> paths) {
    GuestAddr object = 0;
    {
        std::lock_guard lock(m_mutex);
        if (!m_free_arrays.empty()) {
            object = m_free_arrays.back();
            m_free_arrays.pop_back();
        }
    }
    if (object == 0) object = m_com.allocate_builtin_object(m, kArrayMethods);
    if (object == 0) return 0;
    std::lock_guard lock(m_mutex);
    m_arrays[object] = ItemArray{std::move(paths), 1};
    return object;
}

// -----------------------------------------------------------------------------
// IFileDialog
// -----------------------------------------------------------------------------

#define FIND_DIALOG(m)                                                   \
    std::shared_ptr<Dialog> dialog;                                      \
    {                                                                    \
        std::lock_guard lock_(m_mutex);                                  \
        const auto found_ = m_dialogs.find((m).arg_ptr(0));              \
        if (found_ == m_dialogs.end()) return com::E_INVALIDARG;         \
        dialog = found_->second;                                         \
    }

u32 ShellItemDialogs::dialog_query_interface(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) return com::E_POINTER;
    FIND_DIALOG(m);
    if (iid_ptr == 0) return com::E_INVALIDARG;
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    const Guid& id = iid.value();
    if (guid_equal(id, IID_IUNKNOWN) || guid_equal(id, IID_IMODALWINDOW) ||
        guid_equal(id, IID_IFILEDIALOG) ||
        guid_equal(id, dialog->save ? IID_IFILESAVEDIALOG : IID_IFILEOPENDIALOG)) {
        {
            std::lock_guard lock(m_mutex);
            ++dialog->refs;
        }
        (void)m.write_ptr(out, self);
        return S_OK;
    }
    // Windows answers these too (file_dialog_probe "answers"); Vitreo has no
    // custom controls, second-generation options, site or owner window yet.
    if (guid_equal(id, IID_IFILEDIALOGCUSTOMIZE)) unimplemented_once("FileDialog: IFileDialogCustomize");
    if (guid_equal(id, IID_IFILEDIALOG2)) unimplemented_once("FileDialog: IFileDialog2");
    if (guid_equal(id, IID_IOBJECTWITHSITE)) unimplemented_once("FileDialog: IObjectWithSite");
    if (guid_equal(id, IID_IOLEWINDOW)) unimplemented_once("FileDialog: IOleWindow");
    (void)m.write_ptr(out, 0);
    return com::E_NOINTERFACE;
}

u32 ShellItemDialogs::dialog_add_ref(Marshaler& m) {
    FIND_DIALOG(m);
    std::lock_guard lock(m_mutex);
    return ++dialog->refs;
}

u32 ShellItemDialogs::dialog_release(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    FIND_DIALOG(m);
    std::vector<Sink> sinks;
    u32 remaining = 0;
    {
        std::lock_guard lock(m_mutex);
        remaining = dialog->refs > 0 ? --dialog->refs : 0;
        if (remaining == 0) {
            sinks = std::move(dialog->sinks);
            m_dialogs.erase(self);
            (dialog->save ? m_free_save_dialogs : m_free_open_dialogs).push_back(self);
        }
    }
    for (const Sink& sink : sinks) {
        const u64 args[1] = {sink.events};
        (void)m_com.call_guest_slot(m, sink.events, 2, args, 1);
    }
    return remaining;
}

u32 ShellItemDialogs::dialog_set_file_types(Marshaler& m) {
    FIND_DIALOG(m);
    const u32 count = static_cast<u32>(m.arg(1));
    const GuestAddr specs = m.arg_ptr(2);
    if (count == 0) return S_OK;                   // measured: no effect
    if (specs == 0) return com::E_INVALIDARG;
    std::vector<std::pair<std::string, std::string>> types;
    const GuestAddr width = m.cpu_is_64bit() ? 8 : 4;
    for (u32 i = 0; i < count; ++i) {
        const GuestAddr entry = specs + i * width * 2;
        const GuestAddr name = m.read_ptr(entry).value_or(0);
        const GuestAddr spec = m.read_ptr(entry + width).value_or(0);
        types.emplace_back(name ? m.read_wide_string(name).value_or("") : "",
                           spec ? m.read_wide_string(spec).value_or("") : "");
    }
    std::lock_guard lock(m_mutex);
    if (dialog->types_set) return E_UNEXPECTED;   // measured: a second call
    dialog->types = std::move(types);
    dialog->types_set = true;
    return S_OK;
}

u32 ShellItemDialogs::dialog_set_file_type_index(Marshaler& m) {
    FIND_DIALOG(m);
    const u32 index = static_cast<u32>(m.arg(1));
    std::lock_guard lock(m_mutex);
    // Measured with two types: 0 -> 1, 5 -> 2 (clamped to 1..count).
    const u32 count = static_cast<u32>(dialog->types.size());
    dialog->type_index = count == 0 ? index : std::clamp(index, 1u, count);
    return S_OK;
}

u32 ShellItemDialogs::dialog_get_file_type_index(Marshaler& m) {
    FIND_DIALOG(m);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_INVALIDARG;
    std::lock_guard lock(m_mutex);
    return m.write_u32(out, dialog->type_index).is_ok() ? S_OK : com::E_INVALIDARG;
}

u32 ShellItemDialogs::dialog_advise(Marshaler& m) {
    FIND_DIALOG(m);
    const GuestAddr events = m.arg_ptr(1);
    const GuestAddr cookie_out = m.arg_ptr(2);
    if (events == 0 || cookie_out == 0) return com::E_INVALIDARG;   // measured (NULL sink)
    const u64 args[1] = {events};
    (void)m_com.call_guest_slot(m, events, 1, args, 1);             // AddRef
    u32 cookie = 0;
    {
        std::lock_guard lock(m_mutex);
        cookie = dialog->next_cookie++;
        dialog->sinks.push_back(Sink{events, cookie});
    }
    (void)m.write_u32(cookie_out, cookie);
    return S_OK;
}

u32 ShellItemDialogs::dialog_unadvise(Marshaler& m) {
    FIND_DIALOG(m);
    const u32 cookie = static_cast<u32>(m.arg(1));
    GuestAddr events = 0;
    {
        std::lock_guard lock(m_mutex);
        const auto found = std::find_if(dialog->sinks.begin(), dialog->sinks.end(),
                                        [&](const Sink& s) { return s.cookie == cookie; });
        if (found == dialog->sinks.end()) return com::E_INVALIDARG;   // measured
        events = found->events;
        dialog->sinks.erase(found);
    }
    const u64 args[1] = {events};
    (void)m_com.call_guest_slot(m, events, 2, args, 1);             // Release
    return S_OK;
}

u32 ShellItemDialogs::dialog_set_options(Marshaler& m) {
    FIND_DIALOG(m);
    u32 options = static_cast<u32>(m.arg(1));
    std::lock_guard lock(m_mutex);
    if (dialog->save && (options & FOS_PICKFOLDERS) != 0) return com::E_INVALIDARG;   // measured
    // Measured: an open dialog picking folders also reports FOS_FILEMUSTEXIST.
    if (!dialog->save && (options & FOS_PICKFOLDERS) != 0) options |= FOS_FILEMUSTEXIST;
    dialog->options = options;
    return S_OK;
}

u32 ShellItemDialogs::dialog_get_options(Marshaler& m) {
    FIND_DIALOG(m);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_INVALIDARG;   // measured
    std::lock_guard lock(m_mutex);
    return m.write_u32(out, dialog->options).is_ok() ? S_OK : com::E_INVALIDARG;
}

u32 ShellItemDialogs::dialog_set_folder(Marshaler& m, bool default_folder) {
    FIND_DIALOG(m);
    const GuestAddr item = m.arg_ptr(1);
    std::string path;
    if (item != 0) {
        const auto resolved = item_path(m, item);
        if (!resolved) return com::E_INVALIDARG;
        path = *resolved;
    }
    std::lock_guard lock(m_mutex);
    (default_folder ? dialog->default_folder : dialog->folder) = path;   // NULL clears (measured S_OK)
    return S_OK;
}

u32 ShellItemDialogs::dialog_get_folder(Marshaler& m) {
    FIND_DIALOG(m);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_INVALIDARG;
    std::string path;
    {
        std::lock_guard lock(m_mutex);
        path = !dialog->folder.empty() ? dialog->folder : dialog->default_folder;
    }
    if (path.empty()) {
        if (capsule::Vfs* vfs = m_com.vfs()) path = canonical_path(vfs->cwd()).value_or("");
    }
    if (path.empty()) path = "C:\\";
    const GuestAddr object = new_item(m, path);
    if (object == 0) return E_OUTOFMEMORY;
    (void)m.write_ptr(out, object);
    return S_OK;
}

u32 ShellItemDialogs::dialog_get_current_selection(Marshaler& m) {
    FIND_DIALOG(m);
    // Only meaningful while the dialog is up; it is never up when a guest
    // thread can call this (Show is synchronous). Measured: E_FAIL, untouched.
    return E_FAIL;
}

u32 ShellItemDialogs::dialog_set_string(Marshaler& m, std::string Dialog::* field) {
    FIND_DIALOG(m);
    const GuestAddr text = m.arg_ptr(1);
    std::string value = text ? m.read_wide_string(text).value_or("") : std::string{};
    std::lock_guard lock(m_mutex);
    (*dialog).*field = std::move(value);   // NULL clears (measured S_OK)
    return S_OK;
}

u32 ShellItemDialogs::dialog_set_file_name(Marshaler& m) {
    FIND_DIALOG(m);
    const GuestAddr text = m.arg_ptr(1);
    std::optional<std::string> value;
    if (text != 0) value = m.read_wide_string(text).value_or("");
    std::lock_guard lock(m_mutex);
    dialog->file_name = std::move(value);
    return S_OK;
}

u32 ShellItemDialogs::dialog_get_file_name(Marshaler& m) {
    FIND_DIALOG(m);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_INVALIDARG;
    std::optional<std::string> name;
    {
        std::lock_guard lock(m_mutex);
        name = dialog->file_name;
    }
    if (!name) {
        (void)m.write_ptr(out, 0);   // measured: E_FAIL with NULL
        return E_FAIL;
    }
    return write_task_string(m, out, *name);
}

u32 ShellItemDialogs::dialog_get_result(Marshaler& m) {
    FIND_DIALOG(m);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_INVALIDARG;
    std::string path;
    {
        std::lock_guard lock(m_mutex);
        if (!dialog->have_result || dialog->results.empty()) return E_UNEXPECTED;   // measured
        path = dialog->results.front();
    }
    const GuestAddr object = new_item(m, path);
    if (object == 0) return E_OUTOFMEMORY;
    (void)m.write_ptr(out, object);
    return S_OK;
}

u32 ShellItemDialogs::dialog_add_place(Marshaler& m) {
    FIND_DIALOG(m);
    // The macOS panel's sidebar is the user's, not the caller's.
    unimplemented_once("FileDialog::AddPlace (NSOpenPanel sidebar is not per-dialog)");
    return S_OK;
}

u32 ShellItemDialogs::dialog_close(Marshaler& m) {
    FIND_DIALOG(m);
    std::lock_guard lock(m_mutex);
    if (dialog->showing) unimplemented_once("FileDialog::Close while the panel is up");
    return S_OK;   // measured before Show
}

u32 ShellItemDialogs::dialog_set_client_guid(Marshaler& m) {
    FIND_DIALOG(m);
    return m.arg_ptr(1) == 0 ? com::E_INVALIDARG : S_OK;
}

u32 ShellItemDialogs::dialog_clear_client_data(Marshaler& m) {
    FIND_DIALOG(m);
    // Nothing is persisted per client GUID; Windows answers E_FAIL when there
    // is nothing to clear (file_dialog_probe).
    return E_FAIL;
}

u32 ShellItemDialogs::dialog_set_filter(Marshaler& m) {
    FIND_DIALOG(m);
    if (m.arg_ptr(1) != 0) unimplemented_once("FileDialog::SetFilter (IShellItemFilter not consulted)");
    return S_OK;
}

u32 ShellItemDialogs::dialog_get_results(Marshaler& m) {
    FIND_DIALOG(m);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_INVALIDARG;
    std::vector<std::string> paths;
    {
        std::lock_guard lock(m_mutex);
        if (!dialog->have_result) {
            (void)m.write_ptr(out, 0);   // measured: E_FAIL with NULL
            return E_FAIL;
        }
        paths = dialog->results;
    }
    const GuestAddr object = new_array(m, std::move(paths));
    if (object == 0) return E_OUTOFMEMORY;
    (void)m.write_ptr(out, object);
    return S_OK;
}

u32 ShellItemDialogs::dialog_get_selected_items(Marshaler& m) {
    FIND_DIALOG(m);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_INVALIDARG;
    (void)m.write_ptr(out, 0);   // measured outside Show: E_FAIL, NULL
    return E_FAIL;
}

u32 ShellItemDialogs::dialog_set_save_as_item(Marshaler& m) {
    FIND_DIALOG(m);
    const auto path = item_path(m, m.arg_ptr(1));
    if (!path) return com::E_INVALIDARG;
    std::lock_guard lock(m_mutex);
    dialog->folder = parent_of(*path);
    dialog->file_name = leaf_of(*path);
    return S_OK;
}

u32 ShellItemDialogs::dialog_property_store(Marshaler& m, const char* method) {
    FIND_DIALOG(m);
    unimplemented_once(method);
    return com::E_NOTIMPL;
}

u32 ShellItemDialogs::dialog_show(Marshaler& m) {
    FIND_DIALOG(m);
    const GuestAddr self = m.arg_ptr(0);
    auto* bridge = m_com.cocoa_bridge();
    windowing::CocoaBridge::ItemDialogRequest request;
    std::vector<Sink> sinks;
    {
        std::lock_guard lock(m_mutex);
        if (dialog->showing) return E_UNEXPECTED;
        dialog->showing = true;
        dialog->have_result = false;
        dialog->results.clear();
        request.save = dialog->save;
        request.pick_folders = (dialog->options & FOS_PICKFOLDERS) != 0;
        request.multi_select = !dialog->save && (dialog->options & FOS_ALLOWMULTISELECT) != 0;
        request.show_hidden = (dialog->options & FOS_FORCESHOWHIDDEN) != 0;
        request.title = dialog->title;
        request.ok_label = dialog->ok_label;
        request.file_name_label = dialog->file_name_label;
        if (dialog->file_name) request.file_name = leaf_of(*dialog->file_name);
        const std::string folder =
            !dialog->folder.empty() ? dialog->folder : dialog->default_folder;
        if (!folder.empty()) request.initial_dir = host_path(folder).value_or("");
        if (!dialog->types.empty()) {
            const u32 index = std::clamp(dialog->type_index == 0 ? 1u : dialog->type_index, 1u,
                                         static_cast<u32>(dialog->types.size()));
            request.extensions = spec_extensions(dialog->types[index - 1].second);
        }
        sinks = dialog->sinks;
    }
    if (capsule::Vfs* vfs = m_com.vfs()) {
        if (request.initial_dir.empty()) request.initial_dir = host_path(vfs->cwd()).value_or("");
        // The capsule is isolated: the panel offers only what the guest's
        // drives contain (user decision, 2026-10-26).
        for (char drive : vfs->mounted_drives()) {
            if (auto root = host_path(std::string(1, drive) + ":\\")) {
                request.allowed_roots.push_back(*root);
            }
        }
    }
    if (!sinks.empty()) {
        unimplemented_once("IFileDialogEvents OnFolderChanging/OnFolderChange/OnSelectionChange/"
                           "OnTypeChange/OnOverwrite are not delivered (only OnFileOk)");
    }
    const auto finish = [&](u32 hr) {
        std::lock_guard lock(m_mutex);
        dialog->showing = false;
        if (hr != S_OK) {
            dialog->have_result = false;
            dialog->results.clear();
        }
        return hr;
    };
    if (bridge == nullptr) {
        unimplemented_once("FileDialog::Show without a Cocoa bridge");
        return finish(E_FAIL);
    }
    for (;;) {
        const auto chosen = bridge->show_item_dialog(request);
        if (!chosen.ok) return finish(HRESULT_CANCELLED);
        std::vector<std::string> paths;
        bool outside = false;
        for (const auto& host : chosen.paths) {
            auto guest = guest_path(host);
            if (!guest) { outside = true; break; }
            std::string path = *guest;
            if (request.save) {
                std::string extension;
                {
                    std::lock_guard lock(m_mutex);
                    extension = dialog->default_extension;
                }
                if (!extension.empty() && leaf_of(path).find('.') == std::string::npos) {
                    path += "." + extension;
                }
            }
            paths.push_back(canonical_path(path).value_or(path));
        }
        if (outside) {
            // The panel refuses locations outside the drives itself; one that
            // still arrives has no Windows path, so it is not a choice.
            std::fprintf(stderr, "[vitreo] FileDialog: selection is outside the capsule's "
                                 "drives; treated as cancel\n");
            return finish(HRESULT_CANCELLED);
        }
        {
            std::lock_guard lock(m_mutex);
            dialog->results = paths;
            dialog->have_result = true;   // GetResult works inside OnFileOk
        }
        bool refused = false;
        for (const Sink& sink : sinks) {
            const u64 args[2] = {sink.events, self};
            if (m_com.call_guest_slot(m, sink.events, 3, args, 2) == S_FALSE) refused = true;
        }
        if (refused) continue;   // Learn OnFileOk: S_FALSE keeps the dialog open
        return finish(S_OK);
    }
}

// -----------------------------------------------------------------------------
// IShellItem
// -----------------------------------------------------------------------------

#define FIND_ITEM(m, path_var)                                            \
    std::string path_var;                                                 \
    {                                                                     \
        std::lock_guard lock_(m_mutex);                                   \
        const auto found_ = m_items.find((m).arg_ptr(0));                 \
        if (found_ == m_items.end()) return com::E_INVALIDARG;            \
        path_var = found_->second.path;                                   \
    }

u32 ShellItemDialogs::item_query_interface(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) return com::E_POINTER;
    FIND_ITEM(m, path);
    if (iid_ptr == 0) return com::E_INVALIDARG;
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    if (guid_equal(iid.value(), IID_IUNKNOWN) || guid_equal(iid.value(), IID_ISHELLITEM)) {
        {
            std::lock_guard lock(m_mutex);
            ++m_items[self].refs;
        }
        (void)m.write_ptr(out, self);
        return S_OK;
    }
    if (guid_equal(iid.value(), IID_ISHELLITEM2)) unimplemented_once("ShellItem: IShellItem2");
    (void)m.write_ptr(out, 0);
    return com::E_NOINTERFACE;
}

u32 ShellItemDialogs::item_add_ref(Marshaler& m) {
    std::lock_guard lock(m_mutex);
    const auto found = m_items.find(m.arg_ptr(0));
    return found == m_items.end() ? 0 : ++found->second.refs;
}

u32 ShellItemDialogs::item_release(Marshaler& m) {
    std::lock_guard lock(m_mutex);
    const auto found = m_items.find(m.arg_ptr(0));
    if (found == m_items.end()) return 0;
    const u32 remaining = found->second.refs > 0 ? --found->second.refs : 0;
    if (remaining == 0) {
        m_free_items.push_back(found->first);
        m_items.erase(found);
    }
    return remaining;
}

u32 ShellItemDialogs::item_bind_to_handler(Marshaler& m) {
    FIND_ITEM(m, path);
    if (const GuestAddr out = m.arg_ptr(4); out != 0) (void)m.write_ptr(out, 0);
    unimplemented_once("IShellItem::BindToHandler");
    return MK_E_NOOBJECT;
}

u32 ShellItemDialogs::item_get_parent(Marshaler& m) {
    FIND_ITEM(m, path);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_INVALIDARG;
    std::string parent;
    if (path == kThisPc) {
        // Its parent is the Desktop namespace root (file_dialog_probe).
        unimplemented_once("IShellItem::GetParent of This PC (the Desktop namespace root)");
        (void)m.write_ptr(out, 0);
        return MK_E_NOOBJECT;
    }
    parent = is_root(path) ? std::string(kThisPc) : parent_of(path);
    const GuestAddr object = new_item(m, parent);
    if (object == 0) return E_OUTOFMEMORY;
    (void)m.write_ptr(out, object);
    return S_OK;
}

u32 ShellItemDialogs::item_get_display_name(Marshaler& m) {
    FIND_ITEM(m, path);
    const u32 sigdn = static_cast<u32>(m.arg(1));
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) return com::E_INVALIDARG;
    u32 hr = S_OK;
    const std::string name = display_name(path, sigdn, hr);
    if (hr != S_OK) {
        (void)m.write_ptr(out, 0);
        return hr;
    }
    return write_task_string(m, out, name);
}

u32 ShellItemDialogs::item_get_attributes(Marshaler& m) {
    FIND_ITEM(m, path);
    const u32 mask = static_cast<u32>(m.arg(1));
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) return com::E_INVALIDARG;
    const u32 attributes = attributes_of(path) & mask;
    (void)m.write_u32(out, attributes);
    return attributes == mask ? S_OK : S_FALSE;   // measured
}

u32 ShellItemDialogs::item_compare(Marshaler& m) {
    FIND_ITEM(m, path);
    const GuestAddr other_item = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(3);
    if (out == 0) return com::E_INVALIDARG;
    const auto other = item_path(m, other_item);
    if (!other) return com::E_INVALIDARG;
    // Measured: equal -> S_OK/0; otherwise S_FALSE with the namespace order:
    // an ancestor first, and among siblings folders before files.
    int order = 0;
    const std::string a = lower(path), b = lower(*other);
    if (a != b) {
        if (a == lower(std::string(kThisPc))) order = -1;
        else if (b == lower(std::string(kThisPc))) order = 1;
        else if (b.rfind(a, 0) == 0) order = -1;
        else if (a.rfind(b, 0) == 0) order = 1;
        else {
            usize common = 0;
            while (common < a.size() && common < b.size() && a[common] == b[common]) ++common;
            const usize base = a.rfind('\\', common == 0 ? 0 : common - 1);
            const auto child = [&](const std::string& full, const std::string& original) {
                const usize end = full.find('\\', base + 1);
                return original.substr(0, end == std::string::npos ? original.size() : end);
            };
            const std::string a_child = child(a, path), b_child = child(b, *other);
            const bool a_folder = (attributes_of(a_child) & SFGAO_FOLDER) != 0;
            const bool b_folder = (attributes_of(b_child) & SFGAO_FOLDER) != 0;
            if (a_folder != b_folder) order = a_folder ? -1 : 1;
            else order = lower(a_child) < lower(b_child) ? -1 : 1;
        }
    }
    (void)m.write_u32(out, static_cast<u32>(order));
    return order == 0 ? S_OK : S_FALSE;
}

// -----------------------------------------------------------------------------
// IShellItemArray
// -----------------------------------------------------------------------------

u32 ShellItemDialogs::array_query_interface(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) return com::E_POINTER;
    if (iid_ptr == 0) return com::E_INVALIDARG;
    const auto iid = m.read_struct<Guid>(iid_ptr);
    std::lock_guard lock(m_mutex);
    const auto found = m_arrays.find(self);
    if (found == m_arrays.end() || iid.is_err()) return com::E_INVALIDARG;
    if (guid_equal(iid.value(), IID_IUNKNOWN) || guid_equal(iid.value(), IID_ISHELLITEMARRAY)) {
        ++found->second.refs;
        (void)m.write_ptr(out, self);
        return S_OK;
    }
    (void)m.write_ptr(out, 0);
    return com::E_NOINTERFACE;
}

u32 ShellItemDialogs::array_add_ref(Marshaler& m) {
    std::lock_guard lock(m_mutex);
    const auto found = m_arrays.find(m.arg_ptr(0));
    return found == m_arrays.end() ? 0 : ++found->second.refs;
}

u32 ShellItemDialogs::array_release(Marshaler& m) {
    std::lock_guard lock(m_mutex);
    const auto found = m_arrays.find(m.arg_ptr(0));
    if (found == m_arrays.end()) return 0;
    const u32 remaining = found->second.refs > 0 ? --found->second.refs : 0;
    if (remaining == 0) {
        m_free_arrays.push_back(found->first);
        m_arrays.erase(found);
    }
    return remaining;
}

u32 ShellItemDialogs::array_unsupported(Marshaler& m, const char* method) {
    unimplemented_once(method);
    (void)m;
    return com::E_NOTIMPL;
}

u32 ShellItemDialogs::array_get_attributes(Marshaler& m) {
    const u32 flags = static_cast<u32>(m.arg(1));
    const u32 mask = static_cast<u32>(m.arg(2));
    const GuestAddr out = m.arg_ptr(3);
    if (out == 0) return com::E_INVALIDARG;
    std::vector<std::string> paths;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_arrays.find(m.arg_ptr(0));
        if (found == m_arrays.end()) return com::E_INVALIDARG;
        paths = found->second.paths;
    }
    if (flags != SIATTRIBFLAGS_AND && flags != SIATTRIBFLAGS_OR && flags != SIATTRIBFLAGS_APPCOMPAT) {
        return com::E_INVALIDARG;
    }
    u32 combined = flags == SIATTRIBFLAGS_OR ? 0u : mask;
    for (const auto& path : paths) {
        const u32 attributes = attributes_of(path) & mask;
        combined = flags == SIATTRIBFLAGS_OR ? (combined | attributes) : (combined & attributes);
    }
    if (paths.empty()) combined = 0;
    (void)m.write_u32(out, combined);
    return combined == mask ? S_OK : S_FALSE;
}

u32 ShellItemDialogs::array_get_count(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return com::E_INVALIDARG;
    std::lock_guard lock(m_mutex);
    const auto found = m_arrays.find(m.arg_ptr(0));
    if (found == m_arrays.end()) return com::E_INVALIDARG;
    (void)m.write_u32(out, static_cast<u32>(found->second.paths.size()));
    return S_OK;
}

u32 ShellItemDialogs::array_get_item_at(Marshaler& m) {
    const u32 index = static_cast<u32>(m.arg(1));
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) return com::E_INVALIDARG;
    std::string path;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_arrays.find(m.arg_ptr(0));
        if (found == m_arrays.end() || index >= found->second.paths.size()) {
            (void)m.write_ptr(out, 0);
            return com::E_INVALIDARG;
        }
        path = found->second.paths[index];
    }
    const GuestAddr object = new_item(m, path);
    if (object == 0) return E_OUTOFMEMORY;
    (void)m.write_ptr(out, object);
    return S_OK;
}

// -----------------------------------------------------------------------------
// SHCreateItemFromParsingName
// -----------------------------------------------------------------------------

u32 ShellItemDialogs::create_item_from_parsing_name(Marshaler& m) {
    const GuestAddr name_ptr = m.arg_ptr(0);
    const GuestAddr iid_ptr = m.arg_ptr(2);
    const GuestAddr out = m.arg_ptr(3);
    if (out == 0) return com::E_INVALIDARG;
    (void)m.write_ptr(out, 0);
    if (name_ptr == 0 || iid_ptr == 0) return com::E_INVALIDARG;
    const auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) return com::E_INVALIDARG;
    if (!guid_equal(iid.value(), IID_IUNKNOWN) && !guid_equal(iid.value(), IID_ISHELLITEM)) {
        if (guid_equal(iid.value(), IID_ISHELLITEM2)) unimplemented_once("ShellItem: IShellItem2");
        return com::E_NOINTERFACE;
    }
    const std::string name = m.read_wide_string(name_ptr).value_or("");
    std::optional<std::string> path;
    if (name.empty() || name == kThisPc) {
        path = std::string(kThisPc);   // measured: "" names This PC
    } else if (name.rfind("::", 0) == 0) {
        unimplemented_once("SHCreateItemFromParsingName: shell namespace parsing names");
    } else {
        path = canonical_path(name);
    }
    if (!path) return HRESULT_FILE_NOT_FOUND;   // measured: missing, relative
    const GuestAddr object = new_item(m, *path);
    if (object == 0) return E_OUTOFMEMORY;
    (void)m.write_ptr(out, object);
    return S_OK;
}

// -----------------------------------------------------------------------------
// Thunks
// -----------------------------------------------------------------------------

void ShellItemDialogs::append_thunks(DllExports& exports) {
    const auto add = [&exports](const char* name, u16 stack_bytes, auto&& fn) {
        exports.entries.push_back(ExportEntry{name, 0,
            [fn = std::forward<decltype(fn)>(fn)](Marshaler& m) { m.set_return(fn(m)); },
            stack_bytes});
    };
    add("Vitreo_FileDialog_QueryInterface", 12, [this](Marshaler& m) { return dialog_query_interface(m); });
    add("Vitreo_FileDialog_AddRef", 4, [this](Marshaler& m) { return dialog_add_ref(m); });
    add("Vitreo_FileDialog_Release", 4, [this](Marshaler& m) { return dialog_release(m); });
    add("Vitreo_FileDialog_Show", 8, [this](Marshaler& m) { return dialog_show(m); });
    add("Vitreo_FileDialog_SetFileTypes", 12, [this](Marshaler& m) { return dialog_set_file_types(m); });
    add("Vitreo_FileDialog_SetFileTypeIndex", 8, [this](Marshaler& m) { return dialog_set_file_type_index(m); });
    add("Vitreo_FileDialog_GetFileTypeIndex", 8, [this](Marshaler& m) { return dialog_get_file_type_index(m); });
    add("Vitreo_FileDialog_Advise", 12, [this](Marshaler& m) { return dialog_advise(m); });
    add("Vitreo_FileDialog_Unadvise", 8, [this](Marshaler& m) { return dialog_unadvise(m); });
    add("Vitreo_FileDialog_SetOptions", 8, [this](Marshaler& m) { return dialog_set_options(m); });
    add("Vitreo_FileDialog_GetOptions", 8, [this](Marshaler& m) { return dialog_get_options(m); });
    add("Vitreo_FileDialog_SetDefaultFolder", 8, [this](Marshaler& m) { return dialog_set_folder(m, true); });
    add("Vitreo_FileDialog_SetFolder", 8, [this](Marshaler& m) { return dialog_set_folder(m, false); });
    add("Vitreo_FileDialog_GetFolder", 8, [this](Marshaler& m) { return dialog_get_folder(m); });
    add("Vitreo_FileDialog_GetCurrentSelection", 8, [this](Marshaler& m) { return dialog_get_current_selection(m); });
    add("Vitreo_FileDialog_SetFileName", 8, [this](Marshaler& m) { return dialog_set_file_name(m); });
    add("Vitreo_FileDialog_GetFileName", 8, [this](Marshaler& m) { return dialog_get_file_name(m); });
    add("Vitreo_FileDialog_SetTitle", 8, [this](Marshaler& m) { return dialog_set_string(m, &Dialog::title); });
    add("Vitreo_FileDialog_SetOkButtonLabel", 8, [this](Marshaler& m) { return dialog_set_string(m, &Dialog::ok_label); });
    add("Vitreo_FileDialog_SetFileNameLabel", 8, [this](Marshaler& m) { return dialog_set_string(m, &Dialog::file_name_label); });
    add("Vitreo_FileDialog_GetResult", 8, [this](Marshaler& m) { return dialog_get_result(m); });
    add("Vitreo_FileDialog_AddPlace", 12, [this](Marshaler& m) { return dialog_add_place(m); });
    add("Vitreo_FileDialog_SetDefaultExtension", 8, [this](Marshaler& m) { return dialog_set_string(m, &Dialog::default_extension); });
    add("Vitreo_FileDialog_Close", 8, [this](Marshaler& m) { return dialog_close(m); });
    add("Vitreo_FileDialog_SetClientGuid", 8, [this](Marshaler& m) { return dialog_set_client_guid(m); });
    add("Vitreo_FileDialog_ClearClientData", 4, [this](Marshaler& m) { return dialog_clear_client_data(m); });
    add("Vitreo_FileDialog_SetFilter", 8, [this](Marshaler& m) { return dialog_set_filter(m); });
    add("Vitreo_FileDialog_GetResults", 8, [this](Marshaler& m) { return dialog_get_results(m); });
    add("Vitreo_FileDialog_GetSelectedItems", 8, [this](Marshaler& m) { return dialog_get_selected_items(m); });
    add("Vitreo_FileDialog_SetSaveAsItem", 8, [this](Marshaler& m) { return dialog_set_save_as_item(m); });
    add("Vitreo_FileDialog_SetProperties", 8, [this](Marshaler& m) { return dialog_property_store(m, "IFileSaveDialog::SetProperties"); });
    add("Vitreo_FileDialog_SetCollectedProperties", 12, [this](Marshaler& m) { return dialog_property_store(m, "IFileSaveDialog::SetCollectedProperties"); });
    add("Vitreo_FileDialog_GetProperties", 8, [this](Marshaler& m) { return dialog_property_store(m, "IFileSaveDialog::GetProperties"); });
    add("Vitreo_FileDialog_ApplyProperties", 20, [this](Marshaler& m) { return dialog_property_store(m, "IFileSaveDialog::ApplyProperties"); });

    add("Vitreo_ShellItem_QueryInterface", 12, [this](Marshaler& m) { return item_query_interface(m); });
    add("Vitreo_ShellItem_AddRef", 4, [this](Marshaler& m) { return item_add_ref(m); });
    add("Vitreo_ShellItem_Release", 4, [this](Marshaler& m) { return item_release(m); });
    add("Vitreo_ShellItem_BindToHandler", 20, [this](Marshaler& m) { return item_bind_to_handler(m); });
    add("Vitreo_ShellItem_GetParent", 8, [this](Marshaler& m) { return item_get_parent(m); });
    add("Vitreo_ShellItem_GetDisplayName", 12, [this](Marshaler& m) { return item_get_display_name(m); });
    add("Vitreo_ShellItem_GetAttributes", 12, [this](Marshaler& m) { return item_get_attributes(m); });
    add("Vitreo_ShellItem_Compare", 16, [this](Marshaler& m) { return item_compare(m); });

    add("Vitreo_ShellItemArray_QueryInterface", 12, [this](Marshaler& m) { return array_query_interface(m); });
    add("Vitreo_ShellItemArray_AddRef", 4, [this](Marshaler& m) { return array_add_ref(m); });
    add("Vitreo_ShellItemArray_Release", 4, [this](Marshaler& m) { return array_release(m); });
    add("Vitreo_ShellItemArray_BindToHandler", 20, [this](Marshaler& m) { return array_unsupported(m, "IShellItemArray::BindToHandler"); });
    add("Vitreo_ShellItemArray_GetPropertyStore", 16, [this](Marshaler& m) { return array_unsupported(m, "IShellItemArray::GetPropertyStore"); });
    add("Vitreo_ShellItemArray_GetPropertyDescriptionList", 16, [this](Marshaler& m) { return array_unsupported(m, "IShellItemArray::GetPropertyDescriptionList"); });
    add("Vitreo_ShellItemArray_GetAttributes", 16, [this](Marshaler& m) { return array_get_attributes(m); });
    add("Vitreo_ShellItemArray_GetCount", 8, [this](Marshaler& m) { return array_get_count(m); });
    add("Vitreo_ShellItemArray_GetItemAt", 12, [this](Marshaler& m) { return array_get_item_at(m); });
    add("Vitreo_ShellItemArray_EnumItems", 8, [this](Marshaler& m) { return array_unsupported(m, "IShellItemArray::EnumItems"); });
}

} // namespace vitreo::winapi
