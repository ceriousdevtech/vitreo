#pragma once
#include <functional>

/// @file ntdll_ext.hpp
/// @brief ntdll.dll extended function stubs (WIN32-086)

#include <vitreo/common.hpp>
#include "capsule/vfs/filesystem.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/system/ntdll/nt_timeout.hpp"

#include <vector>
#include <cstring>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <span>

namespace vitreo::capsule {
class Vfs;
enum class VfsError;
enum class AccessMode : u32;
enum class ShareMode : u32;
enum class CreationDisposition;
} // namespace vitreo::capsule

namespace vitreo::winapi {

class Marshaler;
class FileManager;
class FileMappingManager;
class ModuleManager;
class ProcessManager;
class SyncManager;
class ThreadManager;
class VirtualMemoryManager;

void impl_RtlCaptureStackBackTrace(Marshaler& m);

// =============================================================================
// Additional NTSTATUS Constants
// =============================================================================

namespace nt_status {

constexpr u32 STATUS_INVALID_PARAMETER     = 0xC000000D;
constexpr u32 STATUS_ACCESS_DENIED         = 0xC0000022;
constexpr u32 STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr u32 STATUS_OBJECT_NAME_INVALID   = 0xC0000033;
constexpr u32 STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
constexpr u32 STATUS_NO_MEMORY             = 0xC0000017;
constexpr u32 STATUS_NOT_IMPLEMENTED       = 0xC0000002;
constexpr u32 STATUS_INVALID_SYSTEM_SERVICE = 0xC000001C;
constexpr u32 STATUS_PROCESS_IS_TERMINATING = 0xC000010A;
constexpr u32 STATUS_SHARING_VIOLATION     = 0xC0000043;
constexpr u32 STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
constexpr u32 STATUS_DISK_FULL             = 0xC000007F;
constexpr u32 STATUS_INVALID_DEVICE_REQUEST = 0xC0000010;
constexpr u32 STATUS_END_OF_FILE           = 0xC0000011;
constexpr u32 STATUS_OBJECT_TYPE_MISMATCH  = 0xC0000024;
constexpr u32 STATUS_ACCESS_VIOLATION      = 0xC0000005;
constexpr u32 STATUS_NO_SUCH_FILE          = 0xC000000F;
constexpr u32 STATUS_BUFFER_OVERFLOW       = 0x80000005;
constexpr u32 STATUS_NO_MORE_FILES         = 0x80000006;
constexpr u32 STATUS_INVALID_PARAMETER_1   = 0xC00000EF;
constexpr u32 STATUS_INVALID_PARAMETER_2   = 0xC00000F0;
constexpr u32 STATUS_INVALID_PARAMETER_3   = 0xC00000F1;
constexpr u32 STATUS_INVALID_FILE_FOR_SECTION = 0xC0000020u;  // ntstatus.h (was typed 0xC000001E)
constexpr u32 STATUS_INVALID_PAGE_PROTECTION  = 0xC0000045u;
constexpr u32 STATUS_NOT_SUPPORTED            = 0xC00000BBu;
constexpr u32 STATUS_DLL_NOT_FOUND            = 0xC0000135u;
constexpr u32 STATUS_PROCEDURE_NOT_FOUND      = 0xC000007Au;
constexpr u32 STATUS_ABANDONED_WAIT_0         = 0x00000080u;
constexpr u32 STATUS_USER_APC                 = 0x000000C0u;
constexpr u32 STATUS_ALERTED                  = 0x00000101u;
constexpr u32 STATUS_TIMEOUT                  = 0x00000102u;
constexpr u32 STATUS_NO_YIELD_PERFORMED       = 0x40000024u;
constexpr u32 STATUS_INVALID_CID              = 0xC000000Bu;

} // namespace nt_status

// =============================================================================
// NT File Operation Constants
// =============================================================================

namespace nt_file {

// CreateDisposition values (different from Win32 dwCreationDisposition!)
constexpr u32 FILE_SUPERSEDE    = 0; // Replace file if exists
constexpr u32 FILE_OPEN         = 1; // Open existing only
constexpr u32 FILE_CREATE       = 2; // Create new only
constexpr u32 FILE_OPEN_IF      = 3; // Open or create
constexpr u32 FILE_OVERWRITE    = 4; // Open existing and truncate
constexpr u32 FILE_OVERWRITE_IF = 5; // Create or truncate

// CreateOptions flags
constexpr u32 FILE_DIRECTORY_FILE     = 0x00000001;
constexpr u32 FILE_WRITE_THROUGH      = 0x00000002;
constexpr u32 FILE_SEQUENTIAL_ONLY    = 0x00000004;
constexpr u32 FILE_NO_INTERMEDIATE_BUFFERING = 0x00000008;
constexpr u32 FILE_SYNCHRONOUS_IO_ALERT = 0x00000010;
constexpr u32 FILE_SYNCHRONOUS_IO_NONALERT = 0x00000020;
constexpr u32 FILE_NON_DIRECTORY_FILE = 0x00000040;
/// Arms the same pending-delete disposition as
/// NtSetInformationFile(FileDispositionInformation): the object is removed
/// once its last handle closes.
constexpr u32 FILE_DELETE_ON_CLOSE    = 0x00001000;

// FILE_INFORMATION_CLASS values accepted by NtQueryDirectoryFile.
constexpr u32 FileDirectoryInformation       = 1;
constexpr u32 FileFullDirectoryInformation   = 2;
constexpr u32 FileBothDirectoryInformation   = 3;
constexpr u32 FileNamesInformation           = 12;
constexpr u32 FileIdBothDirectoryInformation = 37;
constexpr u32 FileIdFullDirectoryInformation = 38;

// IO_STATUS_BLOCK Information field values
constexpr u32 FILE_SUPERSEDED    = 0; // File was superseded
constexpr u32 FILE_OPENED        = 1; // File was opened
constexpr u32 FILE_CREATED       = 2; // File was created
constexpr u32 FILE_OVERWRITTEN   = 3; // File was overwritten
constexpr u32 FILE_EXISTS        = 4; // File exists (error case)
constexpr u32 FILE_DOES_NOT_EXIST = 5; // File does not exist (error case)

} // namespace nt_file

// =============================================================================
// NtdllExtManager Class
// =============================================================================

class NtdllExtManager {
public:
    NtdllExtManager();
    ~NtdllExtManager() = default;

    // No copy
    NtdllExtManager(const NtdllExtManager&) = delete;
    NtdllExtManager& operator=(const NtdllExtManager&) = delete;

    // -------------------------------------------------------------------------
    // VFS/Handle Integration (for NtCreateFile, etc.)
    // -------------------------------------------------------------------------

    /// Set optional VFS reference for file operations
    void set_vfs(capsule::Vfs* vfs) { m_vfs = vfs; }

    /// Set optional HandleTable reference for handle allocation
    void set_handle_table(HandleTable* handles) { m_handles = handles; }

    /// Share kernel32's process-wide file owner so native queries accept
    /// handles opened by CreateFile as well as by NtCreateFile.
    void set_file_manager(FileManager* manager) noexcept {
        m_file_manager = manager;
    }

    [[nodiscard]] FileManager* file_manager() const noexcept {
        return m_file_manager;
    }

    [[nodiscard]] HandleTable* handle_table() const noexcept { return m_handles; }

    /// Share kernel32's section/view owner with the NT mapping frontend.
    void set_file_mapping_manager(FileMappingManager* manager) noexcept {
        m_file_mapping_manager = manager;
    }

    [[nodiscard]] FileMappingManager* file_mapping_manager() const noexcept {
        return m_file_mapping_manager;
    }

    /// Share kernel32 SyncManager so NtQuerySemaphore can read live counts.
    /// NtDeviceIoControlFile on a socket handle goes to the Winsock layer,
    /// which answers the AFD driver's requests. Returns the NTSTATUS when the
    /// handle is a socket, nullopt otherwise. Arguments: (marshaler, handle,
    /// code, input, input length, output, output length, IoStatusBlock,
    /// ApcContext, Event, ApcRoutine).
    using SocketDeviceControl = std::function<std::optional<u32>(Marshaler&, Handle, u32, GuestAddr, u32,
                                                                 GuestAddr, u32, GuestAddr, GuestAddr, Handle,
                                                                 GuestAddr)>;
    void set_socket_device_control(SocketDeviceControl handler) { m_socket_device_control = std::move(handler); }
    [[nodiscard]] const SocketDeviceControl& socket_device_control() const noexcept {
        return m_socket_device_control;
    }
    void set_sync_manager(SyncManager* sync) noexcept { m_sync_manager = sync; }
    void set_thread_manager(ThreadManager* threads) noexcept {
        m_thread_manager = threads;
    }
    void set_virtual_memory_manager(VirtualMemoryManager* memory) noexcept {
        m_virtual_memory_manager = memory;
    }

    void set_module_manager(ModuleManager* modules) noexcept {
        m_module_manager = modules;
    }
    [[nodiscard]] ModuleManager* module_manager() const noexcept {
        return m_module_manager;
    }
    /// RtlUnwindEx ends the process when an unwind finds no target frame
    /// (its second-chance raise).
    void set_process_manager(ProcessManager* processes) noexcept {
        m_process_manager = processes;
    }
    [[nodiscard]] ProcessManager* process_manager() const noexcept {
        return m_process_manager;
    }

    [[nodiscard]] u32 register_dll_notification(
        u32 flags, GuestAddr callback, GuestAddr context,
        GuestAddr* cookie);
    [[nodiscard]] u32 unregister_dll_notification(GuestAddr cookie);

    /// LdrLockLoaderLock / LdrUnlockLoaderLock — process loader critical section.
    void impl_LdrLockLoaderLock(Marshaler& m);
    void impl_LdrUnlockLoaderLock(Marshaler& m);

    /// LdrLoadDll / LdrGetProcedureAddress — NT loader entry points used via
    /// GetProcAddress("ntdll").
    void impl_LdrLoadDll(Marshaler& m);
    void impl_LdrGetProcedureAddress(Marshaler& m);

    [[nodiscard]] SyncManager* sync_manager() const noexcept {
        return m_sync_manager;
    }
    [[nodiscard]] ThreadManager* thread_manager() const noexcept {
        return m_thread_manager;
    }
    [[nodiscard]] VirtualMemoryManager* virtual_memory_manager() const noexcept {
        return m_virtual_memory_manager;
    }

    /// Set the current process image filename (for example "myapp.exe")
    /// so NtQuerySystemInformation(SystemProcessInformation) can list the
    /// current process by its real name. App-agnostic.
    void set_main_image_name(std::string name) { m_main_image_name = std::move(name); }

    /// Get the current process image filename (may be empty).
    [[nodiscard]] const std::string& main_image_name() const noexcept { return m_main_image_name; }

    /// Check if file operations are available
    [[nodiscard]] bool has_file_support() const { return m_vfs != nullptr && m_handles != nullptr; }

    // -------------------------------------------------------------------------
    // RtlNtStatusToDosError
    // -------------------------------------------------------------------------

    /// Convert an NTSTATUS code to a Win32 error code.
    [[nodiscard]] u32 nt_status_to_dos_error(u32 status) const;

    // -------------------------------------------------------------------------
    // RtlGetVersion
    // -------------------------------------------------------------------------

    /// Version information returned by RtlGetVersion.
    struct VersionInfo {
        u32 major       = 10;
        u32 minor       = 0;
        u32 build       = 19041;
        u32 platform_id = 2;
    };

    /// Return synthetic Windows version info.
    [[nodiscard]] VersionInfo get_version() const;

    // -------------------------------------------------------------------------
    // NtQueryVirtualMemory (basic info)
    // -------------------------------------------------------------------------

    struct MemoryBasicInfo {
        u64 base_address;
        u64 region_size;
        u32 state;    // MEM_COMMIT=0x1000, MEM_FREE=0x10000, MEM_RESERVE=0x2000
        u32 protect;  // PAGE_READWRITE=0x04, etc.
        u32 type;     // MEM_PRIVATE=0x20000
    };

    /// Query virtual memory information for a given address.
    [[nodiscard]] u32 query_virtual_memory(u64 address, MemoryBasicInfo& info) const;

    // -------------------------------------------------------------------------
    // NtQuerySystemInformation
    // -------------------------------------------------------------------------

    struct SystemBasicInfo {
        u32 reserved;
        u32 timer_resolution;
        u32 page_size;
        u32 number_of_physical_pages;
        u32 lowest_physical_page_number;
        u32 highest_physical_page_number;
        u32 allocation_granularity;
        u64 minimum_user_mode_address;
        u64 maximum_user_mode_address;
        u64 active_processors_affinity_mask;
        u8  number_of_processors;
    };

    /// Query system information by class.
    [[nodiscard]] u32 query_system_info(u32 info_class, void* buffer,
                                        u32 length, u32* return_length) const;

    // -------------------------------------------------------------------------
    // NtClose
    // -------------------------------------------------------------------------

    /// Close an NT handle (stub).
    [[nodiscard]] u32 nt_close(u64 handle);

    // -------------------------------------------------------------------------
    // NtDelayExecution
    // -------------------------------------------------------------------------

    /// Delay execution for a given interval in 100-nanosecond units.
    [[nodiscard]] u32 nt_delay_execution(bool alertable, i64 delay_100ns) const;

    // -------------------------------------------------------------------------
    // NtWaitForSingleObject
    // -------------------------------------------------------------------------

    /// Wait on one object until it is signaled or the NT timeout passes.
    [[nodiscard]] u32 nt_wait_for_single_object(
        Handle handle, bool alertable,
        const nt_timeout::Deadline& deadline) const;

    // -------------------------------------------------------------------------
    // NtCreateFile
    // -------------------------------------------------------------------------

    /// Result of NtCreateFile operation
    struct CreateFileResult {
        u32 status;        ///< NTSTATUS code
        Handle handle;     ///< Resulting handle (valid only if status == STATUS_SUCCESS)
        u32 information;   ///< IO_STATUS_BLOCK.Information value
    };

    /// Create or open a file through the VFS
    /// @param path VFS-style path (C:\\path\\to\\file.txt format)
    /// @param desired_access Access mask (GENERIC_READ, etc.)
    /// @param file_attributes File attributes (ignored for now)
    /// @param share_access Share mode flags
    /// @param create_disposition NT create disposition (FILE_OPEN, FILE_CREATE, etc.)
    /// @param create_options NT create options (FILE_DIRECTORY_FILE, etc.)
    /// @return CreateFileResult with status, handle, and information
    [[nodiscard]] CreateFileResult nt_create_file(
        std::string_view path,
        u32 desired_access,
        u32 file_attributes,
        u32 share_access,
        u32 create_disposition,
        u32 create_options);

    // -------------------------------------------------------------------------
    // NtQueryDirectoryFile
    // -------------------------------------------------------------------------

    struct QueryDirectoryResult {
        u32 status = 0; // STATUS_SUCCESS
        u32 information = 0;
    };

    /// Enumerate a directory through the VFS using the scan cursor owned by
    /// the NT file object. The optional UTF-16 expression is captured only on
    /// the first query for a handle, as required by NtQueryDirectoryFile.
    [[nodiscard]] QueryDirectoryResult nt_query_directory_file(
        Handle handle,
        std::span<u8> output,
        u32 information_class,
        bool return_single_entry,
        const std::optional<std::vector<u16>>& file_name,
        bool restart_scan);

    /// True after the first directory query captured this handle's expression.
    [[nodiscard]] bool directory_query_started(Handle handle) const;

    /// Whether an NtCreateFile handle was opened without synchronous-I/O flags.
    [[nodiscard]] bool is_asynchronous_file_handle(Handle handle) const;

    /// Close callback used by the process HandleTable for NT-created files.
    [[nodiscard]] bool on_handle_close(Handle handle, HandleType type) noexcept;

    /// Drop native directory cursor state when another file frontend owns the
    /// closing handle.
    void forget_directory_query(Handle handle) noexcept;

    [[nodiscard]] bool owns_file_handle(Handle handle) const noexcept;

    // -------------------------------------------------------------------------
    // NtReadFile / NtWriteFile
    // -------------------------------------------------------------------------

    /// Read data from a file handle
    /// @param handle Win32 handle to read from
    /// @param buffer Host buffer to read into
    /// @param length Number of bytes to read
    /// @param byte_offset File offset (-1 = use current position)
    /// @param bytes_read [out] Number of bytes actually read
    /// @return NTSTATUS code
    [[nodiscard]] u32 nt_read_file(
        Handle handle,
        std::span<u8> buffer,
        i64 byte_offset,
        u32& bytes_read);

    /// Write data to a file handle
    /// @param handle Win32 handle to write to
    /// @param buffer Host buffer containing data to write
    /// @param byte_offset File offset (-1 = use current position)
    /// @param bytes_written [out] Number of bytes actually written
    /// @return NTSTATUS code
    [[nodiscard]] u32 nt_write_file(
        Handle handle,
        std::span<const u8> buffer,
        i64 byte_offset,
        u32& bytes_written);

    /// Truncate/extend a file to the current file pointer position.
    /// Implements the semantics of Win32 SetEndOfFile.
    /// @param handle Win32 handle
    /// @return NTSTATUS code
    [[nodiscard]] u32 nt_set_end_of_file(Handle handle);

    /// NtSetInformationFile — set file metadata (EndOfFileInformation, etc.)
    /// @param handle  Win32 handle
    /// @param info_class  FILE_INFORMATION_CLASS enum value
    /// @param info_buffer Pointer to the information structure
    /// @param info_length Length of info_buffer in bytes
    /// @return NTSTATUS code
    [[nodiscard]] u32 nt_set_information_file(Handle handle, u32 info_class,
                                              const u8* info_buffer, u32 info_length);

    /// NtQueryInformationFile — query file metadata (StandardInformation, etc.)
    /// @param handle  Win32 handle
    /// @param info_class  FILE_INFORMATION_CLASS enum value
    /// @param info_buffer Output buffer
    /// @param info_length Length of info_buffer in bytes
    /// @return NTSTATUS code
    /// @param bytes_written Optional; receives the bytes actually produced,
    ///        which the IO_STATUS_BLOCK reports as Information.
    [[nodiscard]] u32 nt_query_information_file(Handle handle, u32 info_class,
                                                u8* info_buffer, u32 info_length,
                                                u32* bytes_written = nullptr);

    /// Arm or clear the pending-delete disposition on an NT file handle this
    /// manager owns. The object is removed when its last handle closes.
    /// @return false when `handle` is not an NT file handle here, so the
    ///         caller can fall back to kernel32's FileManager.
    [[nodiscard]] bool set_delete_disposition(Handle handle,
                                              bool delete_on_close);

    /// Guest path an NT-created handle was opened on, or empty when this
    /// manager did not create it.
    [[nodiscard]] std::string nt_path_for_handle(Handle handle) const {
        std::lock_guard<std::mutex> lock(m_file_state_mutex);
        auto it = m_handle_paths.find(handle);
        return it != m_handle_paths.end() ? it->second : std::string{};
    }

    /// Look up the VFS handle for a file handle. NT and Win32 file handles
    /// share one namespace: a handle from CreateFileW is as valid for
    /// NtReadFile / NtWriteFile / NtQuery|SetInformationFile as one from
    /// NtCreateFile (Rust's std writes CreateFileW handles with NtWriteFile).
    [[nodiscard]] std::optional<u32> vfs_handle_for(Handle handle) const;

    /// NtWriteFile ByteOffset values with HighPart -1 (wdm.h; measured).
    static constexpr i64 kWriteAtEndOfFile = -1;        // FILE_WRITE_TO_END_OF_FILE
    static constexpr i64 kWriteAtCurrentPosition = -2;  // FILE_USE_FILE_POINTER_POSITION

    // -------------------------------------------------------------------------
    // Rtl Memory Operations
    // -------------------------------------------------------------------------

    static void rtl_copy_memory(void* dst, const void* src, usize length);
    static void rtl_zero_memory(void* dst, usize length);
    static void rtl_move_memory(void* dst, const void* src, usize length);
    static void rtl_fill_memory(void* dst, usize length, u8 fill);

private:
    // -------------------------------------------------------------------------
    // VFS/Handle Integration
    // -------------------------------------------------------------------------

    capsule::Vfs* m_vfs = nullptr;       ///< VFS reference (optional, for file ops)
    HandleTable* m_handles = nullptr;    ///< Handle table (optional, for file ops)
    FileManager* m_file_manager = nullptr;
    SocketDeviceControl m_socket_device_control;
    FileMappingManager* m_file_mapping_manager = nullptr;
    SyncManager* m_sync_manager = nullptr;
    ThreadManager* m_thread_manager = nullptr;
    VirtualMemoryManager* m_virtual_memory_manager = nullptr;
    ModuleManager* m_module_manager = nullptr;
    ProcessManager* m_process_manager = nullptr;

    /// Filename (no path) of the current process image; populated at startup.
    std::string m_main_image_name;

    /// Tracks Win32 Handle → VFS FileHandle for cleanup
    std::unordered_map<Handle, u32> m_vfs_handles;

    /// Guest path each NT file handle was opened on. NtCreateFile handles live
    /// in this manager rather than FileManager, so delete-on-close needs its
    /// own name tracking — the VFS deletes by path.
    std::unordered_map<Handle, std::string> m_handle_paths;

    /// NT handles whose pending-delete disposition is armed.
    std::unordered_set<Handle> m_delete_pending;

    /// Handles opened with FILE_SYNCHRONOUS_IO_ALERT/NONALERT.
    std::unordered_set<Handle> m_synchronous_file_handles;

    struct DirectoryQueryState {
        /// Every entry the directory holds, in enumeration order, including
        /// "." and "..". Retained unfiltered so a later call that supplies a
        /// new FileName mask can be answered without re-reading the directory.
        std::vector<capsule::DirectoryEntry> all_entries;
        /// `all_entries` filtered by `expression`.
        std::vector<capsule::DirectoryEntry> entries;
        std::optional<std::vector<u16>> expression;
        usize next_index = 0;
    };

    /// Serializes NT file ownership and its per-file-object query cursor.
    mutable std::mutex m_file_state_mutex;
    std::unordered_map<Handle, DirectoryQueryState> m_directory_queries;

    /// Process-wide loader lock (recursive, same contract as LdrpLoaderLock).
    std::recursive_mutex m_loader_lock;
    u32 m_loader_lock_depth{0};
    u32 m_loader_lock_serial{1};

    // -------------------------------------------------------------------------
    // Internal Helpers
    // -------------------------------------------------------------------------

    /// Convert NT DesiredAccess to VFS AccessMode
    [[nodiscard]] static capsule::AccessMode to_access_mode(u32 desired_access) noexcept;

    /// Convert NT ShareAccess to VFS ShareMode
    [[nodiscard]] static capsule::ShareMode to_share_mode(u32 share_access) noexcept;

    /// Convert NT CreateDisposition to VFS CreationDisposition
    [[nodiscard]] static capsule::CreationDisposition to_disposition(u32 create_disposition) noexcept;

    /// Convert VfsError to NTSTATUS
    [[nodiscard]] static u32 vfs_error_to_ntstatus(capsule::VfsError err) noexcept;
};

// =============================================================================
// Export Factory
// =============================================================================

/// Create ntdll extended exports
/// @param mgr NtdllExtManager for extended ntdll functions
[[nodiscard]] DllExports make_ntdll_ext_exports(NtdllExtManager& mgr);

/// VOID RtlZeroMemory(PVOID Destination, SIZE_T Length) and
/// VOID RtlMoveMemory(PVOID Destination, const VOID* Source, SIZE_T Length):
/// ntdll's handlers, which kernel32 exports under the same names.
void impl_RtlZeroMemory(Marshaler& m);
void impl_RtlMoveMemory(Marshaler& m);

} // namespace vitreo::winapi
