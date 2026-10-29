#pragma once
/// @file shell_paths.hpp
/// @brief shell32 path helpers (WIN32-071)

#include "winapi/system/shell32/notify_icons.hpp"
#include <vitreo/common.hpp>
#include "winapi/common/elevation.hpp"
#include "winapi/core/thunk_registry.hpp"

#include <string>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vitreo::capsule { class Vfs; }
namespace vitreo::windowing { class CocoaBridge; }
namespace vitreo { class GuestMemory; }

namespace vitreo::winapi {

class Marshaler;
class ProcessManager;
class ComCtl32Manager;
class ComTaskMemoryManager;
class GlobalLocalManager;

// CSIDL values
namespace csidl {
    constexpr i32 CSIDL_DESKTOP = 0x0000;
    constexpr i32 CSIDL_PROGRAMS = 0x0002;
    constexpr i32 CSIDL_PERSONAL = 0x0005;    // Documents
    constexpr i32 CSIDL_STARTUP = 0x0007;
    constexpr i32 CSIDL_STARTMENU = 0x000B;
    constexpr i32 CSIDL_DESKTOPDIRECTORY = 0x0010;
    constexpr i32 CSIDL_DRIVES = 0x0011;
    constexpr i32 CSIDL_FONTS = 0x0014;
    constexpr i32 CSIDL_COMMON_STARTMENU = 0x0016;
    constexpr i32 CSIDL_COMMON_PROGRAMS = 0x0017;
    constexpr i32 CSIDL_COMMON_STARTUP = 0x0018;
    constexpr i32 CSIDL_COMMON_DESKTOPDIRECTORY = 0x0019;
    constexpr i32 CSIDL_APPDATA = 0x001A;      // Roaming
    constexpr i32 CSIDL_LOCAL_APPDATA = 0x001C;
    constexpr i32 CSIDL_COMMON_APPDATA = 0x0023;
    constexpr i32 CSIDL_WINDOWS = 0x0024;
    constexpr i32 CSIDL_SYSTEM = 0x0025;
    constexpr i32 CSIDL_PROGRAM_FILES = 0x0026;
    constexpr i32 CSIDL_PROGRAM_FILESX86 = 0x002A;
    constexpr i32 CSIDL_PROGRAM_FILES_COMMON = 0x002B;
    constexpr i32 CSIDL_PROGRAM_FILES_COMMONX86 = 0x002C;
    constexpr i32 CSIDL_COMMON_DOCUMENTS = 0x002E;
}

// HRESULT values
namespace hr {
    constexpr u32 S_OK = 0;
    constexpr u32 E_INVALIDARG = 0x80070057;
    constexpr u32 E_FAIL = 0x80004005;
    constexpr u32 E_OUTOFMEMORY = 0x8007000E;
}

// ERROR codes for SHCreateDirectoryExW
namespace sh_err {
    constexpr u32 ERROR_SUCCESS = 0;
    constexpr u32 ERROR_PATH_NOT_FOUND = 3;
    constexpr u32 ERROR_BAD_NETPATH = 53;
    constexpr u32 ERROR_BAD_PATHNAME = 161;
    constexpr u32 ERROR_ALREADY_EXISTS = 183;
    constexpr u32 ERROR_FILENAME_EXCED_RANGE = 206;
}

/// Shell path manager
class ShellPathManager {
public:
    /// (file, parameters, directory, nShow, runas, hProcess out, error out)
    using ExecutableLauncher =
        std::function<bool(const std::string&, const std::string&,
                           const std::string&, i32, bool, u32&, u32&)>;

    /// Constructor with VFS and CocoaBridge for folder browsing
    ShellPathManager(capsule::Vfs& vfs,
                     windowing::CocoaBridge& bridge,
                     GuestMemory& memory,
                     ProcessManager& process_mgr,
                     ComTaskMemoryManager& task_memory,
                     const GuestElevationState* elevation = nullptr);

    /// SHGetFolderPathW: resolve a CSIDL (table generated from the Windows
    /// oracle, tools/oracle_probe/gen_known_folders.py). With `verify` a
    /// folder that is not present fails with HRESULT_FROM_WIN32(
    /// ERROR_FILE_NOT_FOUND) unless CSIDL_FLAG_DONT_VERIFY is set;
    /// CSIDL_FLAG_CREATE creates it. `is64` selects the native or WOW64 view.
    [[nodiscard]] u32 get_folder_path(i32 csidl, std::wstring& path,
                                      bool verify = false, bool is64 = true) const;

    /// SHGetKnownFolderPath by KNOWNFOLDERID bytes, honouring KF_FLAG_CREATE
    /// and KF_FLAG_DONT_VERIFY.
    [[nodiscard]] u32 get_known_folder_path_by_guid(const u8 (&guid)[16], u32 flags,
                                                    bool is64,
                                                    std::wstring& path) const;

    /// Known folder by its FOLDERID_ suffix (no verification).
    [[nodiscard]] u32 get_known_folder_path(const std::string& folder_name,
                                             std::wstring& path) const;

    /// CommandLineToArgvW parsing rules (Learn + shell_paths_probe argv*).
    /// `exe_path` is argv[0] for an empty command line.
    void command_line_to_argv(const std::u16string& cmd_line,
                              const std::u16string& exe_path,
                              std::vector<std::u16string>& args) const;

    /// LocalAlloc/LocalFree for results the guest frees with LocalFree.
    void set_local_memory(GlobalLocalManager* local) noexcept { m_local_memory = local; }

    // HDROP: an HGLOBAL holding DROPFILES {pFiles, pt, fNC, fWide} and a
    // double-null-terminated file list (Learn DROPFILES; measured
    // tools/oracle_probe/shell_hdrop_probe.expected, identical x86 / x64).
    /// DragQueryPoint: the drop point, and TRUE for a client-area drop.
    [[nodiscard]] u32 drag_query_point(Marshaler& m, u64 hdrop, GuestAddr point);
    /// DragQueryFileA/W: the file count (index 0xFFFFFFFF), a name's length
    /// (NULL buffer) or the characters copied, truncated to cch - 1.
    [[nodiscard]] u32 drag_query_file(Marshaler& m, u64 hdrop, u32 index,
                                      GuestAddr buffer, u32 cch, bool wide);
    /// DragFinish: releases the HDROP.
    void drag_finish(u64 hdrop);
    [[nodiscard]] ComTaskMemoryManager& task_memory() noexcept { return m_task_memory; }
    /// Win32 path of the process image (argv[0] for an empty command line).
    void set_executable_path_provider(std::function<std::string()> provider) {
        m_executable_path = std::move(provider);
    }

    /// CommandLineToArgvW — full guest-memory implementation
    /// Parses the command line, allocates guest memory for the result,
    /// and returns the guest address of the argv array.
    /// @param m Marshaler for reading/writing guest memory
    /// @param cmd_ptr Guest address of the wide command line
    /// @param num_args_ptr Guest address to write argc
    /// @return Guest address of the argv array (one LocalAlloc block), or 0
    ///         with the last error set
    [[nodiscard]] GuestAddr command_line_to_argv_guest(Marshaler& m,
                                                        GuestAddr cmd_ptr,
                                                        GuestAddr num_args_ptr);

    /// SHCreateDirectoryExW
    /// @return ERROR_SUCCESS or ERROR_ALREADY_EXISTS
    [[nodiscard]] u32 create_directory_ex(std::string_view utf8_path);

    /// Browse for folder — shows native folder picker constrained to VFS.
    /// @param title Dialog title/prompt
    /// @return PIDL (actually a guest address to allocated path data), or 0 if cancelled
    [[nodiscard]] GuestAddr browse_for_folder(const std::wstring& title);

    /// Get path from PIDL returned by browse_for_folder.
    /// @param pidl PIDL from browse_for_folder
    /// @param[out] path Windows-style path
    /// @return true if valid, false if invalid PIDL
    [[nodiscard]] bool get_path_from_pidl(GuestAddr pidl, std::wstring& path) const;

    /// Free a PIDL allocated by browse_for_folder.
    void free_pidl(GuestAddr pidl);

    /// ILCreateFromPathW: a task-allocated PIDL for an existing item, or 0
    /// with the file-system last error.
    [[nodiscard]] GuestAddr il_create_from_path(const std::string& utf8_path);

    /// SHFree (shell32 ordinal 195). NULL is a no-op. A pointer this
    /// process allocated through the shell IMalloc is released; any other
    /// address is left alone.
    void sh_free(GuestAddr ptr);

    /// SHFileOperationW — file operations with VFS integration
    /// @param m Marshaler for guest memory access
    /// @param file_op_ptr Guest address of SHFILEOPSTRUCTW
    /// @return 0 on success, non-zero error code on failure
    [[nodiscard]] int file_operation(Marshaler& m, GuestAddr file_op_ptr);

    /// FO_COPY rules: a single destination that is not a directory is the
    /// exact target name; a directory destination receives each source's
    /// leaf name. https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shfileoperationa
    [[nodiscard]] int copy_operation(
        const std::vector<std::string>& sources,
        const std::vector<std::string>& destinations);

    /// Expand DOS wildcards in the leaf component of each SHFileOperation
    /// source spec. Windows accepts a pattern there ("dir\\*"), and a caller
    /// that gets no expansion sees the literal path fail to open — installers
    /// react to that by retrying forever rather than by failing.
    /// @param[out] had_wildcard true when any spec held '*' or '?'; a wildcard
    ///        source always means the destination is a directory.
    /// @return the matching paths, with non-wildcard specs passed through
    [[nodiscard]] std::vector<std::string> expand_wildcard_sources(
        const std::vector<std::string>& sources, bool& had_wildcard);

    /// Copy a file, or a whole directory tree, through the capsule VFS.
    [[nodiscard]] bool copy_tree(const std::string& src, const std::string& dst);

    /// Delete a file, or a whole directory tree, through the capsule VFS.
    /// FO_DELETE on a directory removes its contents; VFS remove() alone only
    /// takes an empty one.
    [[nodiscard]] bool remove_tree(const std::string& path);

    /// Create a directory and any missing parents.
    [[nodiscard]] bool ensure_directory(const std::string& path);

    /// SHGetFileInfoW — stub, always succeeds
    /// @return 1 (success)
    [[nodiscard]] u64 get_file_info(Marshaler& m) const;

    /// SHGetFileInfoA — ANSI structure variant.
    [[nodiscard]] u64 get_file_info_ansi(Marshaler& m);

    /// SHGetSpecialFolderLocation
    /// @param csidl CSIDL value
    /// @param[out] pidl Receives allocated PIDL guest address
    /// @return S_OK or E_INVALIDARG
    [[nodiscard]] u32 get_special_folder_location(i32 csidl, GuestAddr& pidl,
                                                  bool is64 = true);

    /// ShellExecuteW — open URLs, files, or folders via native macOS APIs.
    /// @param operation  "open", "explore", "find", etc. (may be empty)
    /// @param file       URL or Windows path to open
    /// @param parameters Command-line parameters (for executables)
    /// @param directory  Working directory
    /// @param show_cmd   SW_SHOW etc.
    /// @return >32 on success, <=32 on error (SE_ERR_*)
    /// Guest path -> host path for anything handed to NSWorkspace.
    /// Empty means unresolvable; callers must report SE_ERR_FNF rather than
    /// pretending success. See the definition.
    [[nodiscard]] std::string host_path_for_shell(const std::string& guest_path);

    [[nodiscard]] u64 shell_execute(const std::string& operation,
                                    const std::string& file,
                                    const std::string& parameters,
                                    const std::string& directory,
                                    i32 show_cmd,
                                    u32* process_handle = nullptr,
                                    const std::string& class_name = {});

    /// Environment lookup for SEE_MASK_DOENVSUBST (%VAR% expansion).
    void set_environment_lookup(
        std::function<std::optional<std::string>(std::string_view)> lookup) {
        m_environment_lookup = std::move(lookup);
    }
    /// ExpandEnvironmentStrings semantics: %NAME% of a defined variable is
    /// replaced, anything else is kept verbatim.
    [[nodiscard]] std::string expand_environment(const std::string& text) const;
    /// CloseHandle for a process handle the caller did not ask for.
    void set_handle_closer(std::function<void(u32)> closer) {
        m_handle_closer = std::move(closer);
    }
    void close_guest_handle(u32 handle) const {
        if (handle != 0 && m_handle_closer) m_handle_closer(handle);
    }
    /// Path behind a PIDL this manager handed out (SEE_MASK_IDLIST).
    [[nodiscard]] bool path_for_pidl(GuestAddr pidl, std::string& path) const {
        return pidl_path(pidl, path);
    }

    void set_executable_launcher(ExecutableLauncher launcher) {
        m_executable_launcher = std::move(launcher);
    }

    /// IsUserAnAdmin — reports the immutable token state for this guest.
    [[nodiscard]] u32 is_user_an_admin() const;

    /// SetCurrentProcessExplicitAppUserModelID — preserve the explicit
    /// taskbar identity for this guest process.
    void set_current_process_explicit_app_user_model_id(Marshaler& m);

    /// GetCurrentProcessExplicitAppUserModelID — return a task-memory-compatible
    /// copy of the explicit identity, if one has been set.
    void get_current_process_explicit_app_user_model_id(Marshaler& m);

    /// The explicit process identity: SetCurrentProcessExplicitAppUserModelID,
    /// or the STARTUPINFO lpTitle a creator passed with STARTF_TITLEISAPPID
    /// (window_propstore_probe "spawn flags=0x1000": the child's
    /// GetCurrentProcessExplicitAppUserModelID returns that title).
    void set_explicit_app_user_model_id(std::u16string app_id);
    [[nodiscard]] std::optional<std::u16string> explicit_app_user_model_id() const;

    /// Access to CocoaBridge for native delegation
    [[nodiscard]] windowing::CocoaBridge& bridge() { return m_bridge; }

    /// Access to VFS
    [[nodiscard]] capsule::Vfs& vfs() { return m_vfs; }

    /// Allocate guest memory from the shell bump allocator
    /// @param memory GuestMemory to map the region
    /// @param size Number of bytes to allocate (will be page-aligned)
    /// @return Guest address, or 0 on failure
    GuestAddr alloc_shell_memory(GuestMemory& memory, u32 size);

    /// Supply the registered thunk table used to construct guest COM vtables.
    void set_thunk_registry(ThunkRegistry& registry) { m_thunk_registry = &registry; }
    void set_comctl32_manager(ComCtl32Manager& manager) { m_comctl32_manager = &manager; }
    /// The notification area behind Shell_NotifyIcon / Shell_NotifyIconGetRect.
    [[nodiscard]] NotifyIconManager& notify_icons() noexcept { return m_notify_icons; }

    /// Guest COM entry points used by the classic shell namespace interfaces.
    void shell_folder_query_interface(Marshaler& m);
    void shell_folder_add_ref(Marshaler& m);
    void shell_folder_release(Marshaler& m);
    void shell_folder_enum_objects(Marshaler& m);
    void shell_folder_bind_to_object(Marshaler& m);
    void shell_folder_compare_ids(Marshaler& m);
    void shell_folder_get_attributes(Marshaler& m);
    void shell_folder_get_display_name(Marshaler& m);
    void enum_id_list_query_interface(Marshaler& m);
    void enum_id_list_add_ref(Marshaler& m);
    void enum_id_list_release(Marshaler& m);
    void enum_id_list_next(Marshaler& m);
    void enum_id_list_skip(Marshaler& m);
    void enum_id_list_reset(Marshaler& m);
    void enum_id_list_clone(Marshaler& m);
    void shell_malloc_query_interface(Marshaler& m);
    void shell_malloc_add_ref(Marshaler& m);
    void shell_malloc_release(Marshaler& m);
    void shell_malloc_alloc(Marshaler& m);
    void shell_malloc_realloc(Marshaler& m);
    void shell_malloc_free(Marshaler& m);
    void shell_malloc_get_size(Marshaler& m);
    void shell_malloc_did_alloc(Marshaler& m);
    void shell_malloc_heap_minimize(Marshaler& m);
    void sh_get_desktop_folder(Marshaler& m);
    void sh_get_malloc(Marshaler& m);

private:
    capsule::Vfs&           m_vfs;
    windowing::CocoaBridge& m_bridge;
    GuestMemory&            m_memory;
    ProcessManager&         m_process_mgr;
    ComTaskMemoryManager&   m_task_memory;
    const GuestElevationState* m_elevation;
    ExecutableLauncher      m_executable_launcher;
    ThunkRegistry*          m_thunk_registry{nullptr};
    ComCtl32Manager*        m_comctl32_manager{nullptr};
    NotifyIconManager       m_notify_icons;
    u32                     m_system_image_list{0};

    GlobalLocalManager*     m_local_memory{nullptr};
    std::function<std::string()> m_executable_path;
    std::function<std::optional<std::string>(std::string_view)> m_environment_lookup;
    std::function<void(u32)> m_handle_closer;

    [[nodiscard]] u32 resolve_folder(const std::wstring& path, bool create,
                                     bool verify) const;

    /// PIDL tracking: guest address → Windows path
    std::unordered_map<GuestAddr, std::wstring> m_pidl_paths;
    GuestAddr m_next_pidl = 0x80000000;  // Fake PIDL range

    struct FolderState {
        std::string path;
        u32 refs{1};
    };
    struct EnumState {
        std::vector<std::string> paths;
        usize cursor{0};
        u32 refs{1};
    };

    std::unordered_map<GuestAddr, FolderState> m_shell_folders;
    std::unordered_map<GuestAddr, EnumState> m_shell_enumerators;
    std::unordered_map<GuestAddr, u32> m_shell_allocations;
    GuestAddr m_desktop_folder{0};
    GuestAddr m_shell_malloc{0};

    mutable std::mutex m_app_user_model_id_mutex;
    std::optional<std::u16string> m_explicit_app_user_model_id;

    [[nodiscard]] GuestAddr create_pidl(const std::string& path);
    [[nodiscard]] GuestAddr create_shell_folder(const std::string& path, bool is64);
    [[nodiscard]] GuestAddr create_shell_enumerator(std::vector<std::string> paths,
                                                    bool is64);
    [[nodiscard]] GuestAddr create_shell_malloc(bool is64);
    [[nodiscard]] bool pidl_path(GuestAddr pidl, std::string& path) const;

    /// Protects Shell allocation, PIDL and namespace-object state. Namespace
    /// methods create/free nested objects while holding this same lock.
    mutable std::recursive_mutex m_shell_allocation_mutex;
    GuestAddr m_shell_alloc_next = 0x5F000000;
};

DllExports create_shell_path_thunks(ShellPathManager& mgr);

} // namespace vitreo::winapi
