#pragma once

/// @file file.hpp
/// @brief CreateFileW / CreateFileA emulation (WIN32-010)
///
/// Bridges Win32 file creation APIs to the Vitreo VFS subsystem.
/// Maps Win32 constants (GENERIC_READ, FILE_SHARE_READ, etc.) to VFS
/// AccessMode/ShareMode/CreationDisposition enums.

#include <vitreo/common.hpp>
#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/common/reaped_workers.hpp"
#include "core/memory/guest_memory.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <string_view>
#include <functional>
#include <memory>
#include <stop_token>
#include <thread>
#include <mutex>
#include <optional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

// Forward declaration for console bridge (WIN32-089)
class ConsoleManager;
class FileMappingManager;
class FileOpsManager;

// =============================================================================
// Win32 File Access Constants (dwDesiredAccess)
// =============================================================================

constexpr u32 GENERIC_READ    = 0x80000000;
constexpr u32 GENERIC_WRITE   = 0x40000000;
constexpr u32 GENERIC_EXECUTE = 0x20000000;
constexpr u32 GENERIC_ALL     = 0x10000000;

// =============================================================================
// Win32 Share Mode Constants (dwShareMode)
// =============================================================================

constexpr u32 FILE_SHARE_READ   = 0x00000001;
constexpr u32 FILE_SHARE_WRITE  = 0x00000002;
constexpr u32 FILE_SHARE_DELETE = 0x00000004;

// =============================================================================
// Win32 Creation Disposition Constants (dwCreationDisposition)
// =============================================================================

constexpr u32 CREATE_NEW        = 1;
constexpr u32 CREATE_ALWAYS     = 2;
constexpr u32 OPEN_EXISTING     = 3;
constexpr u32 OPEN_ALWAYS       = 4;
constexpr u32 TRUNCATE_EXISTING = 5;

// =============================================================================
// Win32 File Flags (dwFlagsAndAttributes)
// =============================================================================

/// Required by CreateFile when opening a directory handle.
constexpr u32 FILE_FLAG_BACKUP_SEMANTICS = 0x02000000;

/// Arms the same pending-delete disposition that
/// SetFileInformationByHandle(FileDispositionInfo) sets: the object is removed
/// once its last handle closes.
constexpr u32 FILE_FLAG_DELETE_ON_CLOSE = 0x04000000;

// =============================================================================
// Win32 SetFilePointer Constants (dwMoveMethod)
// =============================================================================

constexpr u32 FILE_BEGIN   = 0;
constexpr u32 FILE_CURRENT = 1;
constexpr u32 FILE_END     = 2;

struct PipeEndpoint;
}  // namespace vitreo::winapi
namespace vitreo::ipc {
class UnixPipe;
}  // namespace vitreo::ipc
namespace vitreo::winapi {

enum class PipeConnectResult : u8 {
    Connected,
    AlreadyConnected,
    Pending,
    TimedOut,
    InvalidHandle,
    /// CancelIo(Ex) stopped a pending overlapped connect; its OVERLAPPED
    /// completes with ERROR_OPERATION_ABORTED.
    Cancelled,
    /// CloseHandle of the handle it was issued on: ERROR_BROKEN_PIPE
    /// (s4_pipe_connect_probe closehandle_packet gle=6d).
    Closed,
    /// DisconnectNamedPipe while the connect was pending: it completes with
    /// STATUS_PIPE_DISCONNECTED, ERROR_PIPE_NOT_CONNECTED — also when the
    /// handle is closed afterwards (pipe_pending_connect_end_probe).
    Disconnected,
};

enum class PipeDisconnectResult : u8 {
    Disconnected,
    NotConnected,
    InvalidHandle,
};

enum class OverlappedPipeIoResult : u8 {
    Completed,
    Pending,
    Failed,
};

enum class OverlappedIoTraceStage : u8 {
    Submitted,
    DataAvailable,
    BufferFilled,
    CompletionPrepared,
    PacketQueued,
    PacketDequeued,
    GuestCallbackObserved,
    Cancelled,
    Failed,
};

struct OverlappedIoTraceEvent {
    OverlappedIoTraceStage stage{OverlappedIoTraceStage::Submitted};
    Handle handle{NULL_HANDLE};
    u64 object_id{0};
    GuestAddr overlapped{0};
    GuestAddr buffer{0};
    u32 requested_bytes{0};
    u32 transferred_bytes{0};
    u32 win32_error{0};
    u32 nt_status{0};
    bool is_64bit{false};
    bool contains_ping{false};
    u8 preview_size{0};
    std::array<u8, 64> preview{};
};

struct NamedPipeConfig {
    bool message_type{false};
    bool message_read_mode{false};
    bool blocking{true};
    u32 input_buffer_size{4096};
    u32 output_buffer_size{4096};
    u32 max_instances{1};
    u32 default_timeout_ms{0};
    bool overlapped{false};
};

// =============================================================================
// FileManager Class
// =============================================================================

/// Bridges Win32 CreateFile APIs to the VFS
///
/// Manages the mapping between Win32 HANDLEs and VFS FileHandles, and
/// translates Win32 constants to VFS enum values.
///
/// Usage:
/// @code
///     FileManager fm(vfs, handles);
///     Handle h = fm.create_file("C:\\test.txt", GENERIC_READ, 0, OPEN_EXISTING, 0);
///     if (h != INVALID_HANDLE) { ... }
/// @endcode
class FileManager {
public:
    /// (handle, bytes, overlapped, is_64bit, nt_status, win_error,
    ///  completed_synchronously). The final flag is true when ReadFile/
    /// WriteFile returned success inline; async worker completions pass false.
    using OverlappedCompletionCallback =
        std::function<void(Handle, u32, GuestAddr, bool, u32, u32, bool)>;
    using OverlappedTraceCallback =
        std::function<void(const OverlappedIoTraceEvent&)>;
    /// Queue a ReadFileEx/WriteFileEx FileIOCompletionRoutine for the issuing
    /// thread. Wired to ThreadManager so SleepEx can deliver it.
    using IoCompletionRoutineQueue =
        std::function<void(GuestAddr pfn, u32 error_code, u32 bytes,
                           GuestAddr overlapped)>;

    /// Construct a FileManager
    /// @param vfs Reference to the VFS subsystem
    /// @param handles Reference to the process handle table
    FileManager(capsule::Vfs& vfs, HandleTable& handles);

    /// Construct a FileManager with guest memory access (for ReadFile/WriteFile)
    /// @param vfs Reference to the VFS subsystem
    /// @param handles Reference to the process handle table
    /// @param memory Reference to guest memory for marshaling read/write buffers
    FileManager(capsule::Vfs& vfs, HandleTable& handles, GuestMemory& memory);

    /// No copy
    FileManager(const FileManager&) = delete;
    FileManager& operator=(const FileManager&) = delete;

    /// No move (contains references)
    FileManager(FileManager&&) = delete;
    FileManager& operator=(FileManager&&) = delete;

    /// Destructor
    ~FileManager();

    // -------------------------------------------------------------------------
    // Internal Accessors (for kernel32 thunks)
    // -------------------------------------------------------------------------

    [[nodiscard]] HandleTable& handle_table() noexcept { return m_handles; }
    [[nodiscard]] const HandleTable& handle_table() const noexcept { return m_handles; }
    [[nodiscard]] GuestMemory* guest_memory() noexcept { return m_memory; }
    [[nodiscard]] const GuestMemory* guest_memory() const noexcept { return m_memory; }

    /// Get the VFS subsystem reference (for file-backed mappings)
    [[nodiscard]] capsule::Vfs& vfs() noexcept { return m_vfs; }

    /// Look up the VFS FileHandle for a Win32 Handle
    /// @return VFS handle, or capsule::INVALID_FILE_HANDLE if not found
    [[nodiscard]] capsule::FileHandle vfs_handle_for(Handle h) const noexcept {
        std::lock_guard<std::mutex> lock(m_file_path_mutex);
        auto it = m_vfs_handles.find(h);
        return (it != m_vfs_handles.end()) ? it->second : capsule::INVALID_FILE_HANDLE;
    }

    /// Guest path `h` was opened on, or an empty string when `h` is not a
    /// tracked file handle. Returns by value so the snapshot stays valid after
    /// the path-state lock is released.
    [[nodiscard]] std::string path_for_handle(Handle h) const {
        std::lock_guard<std::mutex> lock(m_file_path_mutex);
        auto it = m_handle_paths.find(h);
        return (it != m_handle_paths.end()) ? it->second : std::string{};
    }

    /// FileRenameInfo succeeded: later delete-on-close must use the new name.
    void note_renamed(Handle h, std::string new_path) {
        std::lock_guard<std::mutex> lock(m_file_path_mutex);
        auto it = m_handle_paths.find(h);
        if (it != m_handle_paths.end()) it->second = std::move(new_path);
    }

    /// Arm or clear the pending-delete disposition on an open file handle.
    ///
    /// Windows removes the object when its last handle closes, so this only
    /// records intent; `on_handle_close` performs the removal. Backs both
    /// FILE_FLAG_DELETE_ON_CLOSE and
    /// SetFileInformationByHandle(FileDispositionInfo).
    ///
    /// @return false when `h` is not a tracked file handle, so callers can
    ///         report ERROR_INVALID_HANDLE instead of a silent success.
    bool set_delete_disposition(Handle h, bool delete_on_close);

    /// True when `path` has a pending delete. Windows fails new opens of a
    /// delete-pending object with ERROR_ACCESS_DENIED until it goes away.
    [[nodiscard]] bool is_delete_pending(std::string_view path) const;
    /// CreateFile hTemplateFile: copy the template's attributes onto a newly
    /// created file (no-op for an invalid template).
    void apply_template_attributes(Handle created, Handle template_file);

    // -------------------------------------------------------------------------
    // Console Bridge (WIN32-089)
    // -------------------------------------------------------------------------

    /// Attach a ConsoleManager so WriteFile can delegate console-handle writes
    /// @param cm Reference to the ConsoleManager
    [[nodiscard]] bool is_character_file(Handle handle) const;

    /// GetFileType's classification of @p handle: FILE_TYPE_DISK (1),
    /// FILE_TYPE_CHAR (2), FILE_TYPE_PIPE (3), or FILE_TYPE_UNKNOWN (0) for a
    /// handle Windows would not recognize. Sets no last error.
    [[nodiscard]] u32 file_type(Handle handle) const;

    void set_console_manager(ConsoleManager& cm) noexcept { m_console = &cm; }

    /// Used by SetEndOfFile to refuse shrinking a file that still has an open
    /// mapping object or mapped view (ERROR_USER_MAPPED_FILE).
    void set_file_mapping_manager(FileMappingManager* mgr) noexcept {
        m_file_mapping_manager = mgr;
    }

    /// Directory-enumeration cursor lives on FileOpsManager; wire it so
    /// CloseHandle / DuplicateHandle keep GetFileInformationByHandleEx state.
    void set_file_ops_manager(FileOpsManager* ops) noexcept { m_file_ops = ops; }

    /// Deliver a synchronously completed OVERLAPPED operation to kernel32's
    /// event and I/O-completion-port machinery.
    void set_overlapped_completion_callback(OverlappedCompletionCallback callback) {
        m_overlapped_completion = std::move(callback);
    }

    /// Optional probe for Winsock SOCKET values (separate ID space from
    /// HandleTable). Used by GetFileType so sockets report FILE_TYPE_PIPE.
    using SocketHandleProbe = std::function<bool(u32)>;
    void set_socket_handle_probe(SocketHandleProbe probe) {
        m_socket_handle_probe = std::move(probe);
    }
    [[nodiscard]] bool socket_handle_is_known(u32 handle) const {
        return m_socket_handle_probe && m_socket_handle_probe(handle);
    }
    void notify_overlapped_completion(Handle handle, u32 bytes_transferred,
                                      GuestAddr overlapped, bool is_64bit,
                                      u32 nt_status = 0,
                                      u32 win_error = 0,
                                      bool completed_synchronously = false) const {
        if (m_overlapped_completion) {
            m_overlapped_completion(handle, bytes_transferred, overlapped,
                                    is_64bit, nt_status, win_error,
                                    completed_synchronously);
        }
    }
    void clear_overlapped_completion_callback() {
        m_overlapped_completion = {};
    }
    void set_overlapped_trace_callback(OverlappedTraceCallback callback) {
        m_overlapped_trace = std::move(callback);
    }
    void clear_overlapped_trace_callback() {
        m_overlapped_trace = {};
    }
    void set_io_completion_routine_queue(IoCompletionRoutineQueue callback) {
        m_io_completion_routine_queue = std::move(callback);
    }
    void clear_io_completion_routine_queue() {
        m_io_completion_routine_queue = {};
    }
    void queue_io_completion_routine(GuestAddr pfn, u32 error_code, u32 bytes,
                                     GuestAddr overlapped) const {
        if (m_io_completion_routine_queue) {
            m_io_completion_routine_queue(pfn, error_code, bytes, overlapped);
        }
    }

    /// Stable process-local identity of the underlying pipe/file object.
    /// Numeric HANDLE aliases must report the same value.
    [[nodiscard]] u64 async_object_id(Handle handle) const noexcept;

    [[nodiscard]] OverlappedPipeIoResult begin_overlapped_pipe_read(
        Handle handle, GuestAddr buffer_addr, u32 bytes_to_read,
        GuestAddr overlapped, bool is_64bit, u32& bytes_read);
    [[nodiscard]] OverlappedPipeIoResult begin_overlapped_pipe_write(
        Handle handle, GuestAddr buffer_addr, u32 bytes_to_write,
        GuestAddr bytes_written_addr, GuestAddr overlapped, bool is_64bit,
        u32& bytes_written);
    void shutdown_overlapped_pipe_io();

    /// Host service threads carrying pended overlapped pipe I/O: at most one
    /// for reads and one for writes, however many operations are pending.
    /// Immediate (already-transferred) completions share one dispatcher and
    /// do not increment this count.
    [[nodiscard]] usize overlapped_pipe_io_worker_count();

    /// Cancel pending overlapped pipe I/O (Microsoft Learn CancelIo /
    /// CancelIoEx). Completions are delivered with ERROR_OPERATION_ABORTED.
    /// @param overlapped 0 = all ops on the handle; else that OVERLAPPED only
    /// @param only_calling_thread CancelIo semantics (issuer thread match)
    /// @return number of operations marked cancelled
    usize cancel_overlapped_pipe_io(
        Handle handle, GuestAddr overlapped, bool only_calling_thread);
    /// cancel_overlapped_pipe_io plus a pending overlapped ConnectNamedPipe
    /// issued on @p handle (CancelIo / CancelIoEx).
    usize cancel_pipe_handle_io(Handle handle, GuestAddr overlapped,
                                bool only_calling_thread);

    /// Cooperative CancelSynchronousIo for a thread blocked in sync pipe I/O
    /// (UnixPipe::read). Returns a shared flag the wait loop observes.
    using SyncIoCancelFlag = std::shared_ptr<std::atomic<bool>>;
    [[nodiscard]] SyncIoCancelFlag begin_sync_io(u32 tid);
    void end_sync_io(u32 tid, const SyncIoCancelFlag& flag);
    /// Mark outstanding sync I/O for @p tid. Returns 0 if none, else 1+.
    [[nodiscard]] usize cancel_synchronous_io(u32 tid);

    // -------------------------------------------------------------------------
    // CreateFile Implementation
    // -------------------------------------------------------------------------

    /// Core CreateFile implementation
    ///
    /// Opens or creates a file through the VFS and allocates a Win32 handle.
    ///
    /// @param path Windows-style file path (e.g., "C:\\folder\\file.txt")
    /// @param desired_access Combination of GENERIC_READ, GENERIC_WRITE, etc.
    /// @param share_mode Combination of FILE_SHARE_READ, FILE_SHARE_WRITE, etc.
    /// @param creation_disposition One of CREATE_NEW, OPEN_EXISTING, etc.
    /// @param flags_and_attrs File attributes and flags (currently unused)
    /// @return Valid Handle on success, INVALID_HANDLE on failure
    [[nodiscard]] Handle create_file(
        std::string_view path,
        u32 desired_access,
        u32 share_mode,
        u32 creation_disposition,
        u32 flags_and_attrs
    );

    /// Give an already-open VFS file a kernel32 identity: a HandleType::File
    /// handle that every kernel32 file API accepts, tracked the way create_file
    /// tracks its own (path, open count, delete-on-close). A Windows CRT
    /// descriptor is a thin layer over such a handle, which _get_osfhandle
    /// returns (Learn, _get_osfhandle / _open_osfhandle).
    ///
    /// @param owns_vfs  true: closing the handle closes @p vfs_handle (a CRT
    ///                  descriptor's own file). false: the file belongs to
    ///                  something else (a FILE* stream), so CloseHandle
    ///                  releases the handle only.
    /// @return the handle, or INVALID_HANDLE when the table is full.
    [[nodiscard]] Handle adopt_vfs_handle(capsule::FileHandle vfs_handle,
                                          std::string_view path,
                                          u32 desired_access, bool owns_vfs);

    /// ReOpenFile: new handle to the same CreateFile object with different
    /// access, share, and flags. Learn (winbase.h) — not DuplicateHandle.
    [[nodiscard]] Handle reopen_file(
        Handle original,
        u32 desired_access,
        u32 share_mode,
        u32 flags_and_attrs
    );

    /// Start/stop a kernel-driver contract implemented by Vitreo's native
    /// device boundary. Unsupported drivers fail honestly.
    [[nodiscard]] bool start_kernel_driver(std::string_view service_name,
                                           std::string_view image_path,
                                           u32& error);
    void stop_kernel_driver(std::string_view service_name);
    [[nodiscard]] bool kernel_driver_running(
        std::string_view service_name) const;

    /// Realize FSCTL_SET_REPARSE_POINT on a directory handle. Only the mount
    /// point (directory junction) tag is supported; the host gets a symlink.
    [[nodiscard]] bool set_reparse_point(Handle handle, GuestAddr input,
                                         u32 input_size);

    /// Dispatch a control request for a typed device handle.
    [[nodiscard]] bool device_io_control(
        Handle handle, u32 control_code, GuestAddr input, u32 input_size,
        GuestAddr output, u32 output_size, u32& bytes_returned);

    /// Create a named-pipe SERVER endpoint over an AF_UNIX socket (cross-process
    /// IPC). Returns a HandleType::Pipe handle, or INVALID_HANDLE on failure.
    [[nodiscard]] Handle create_named_pipe_server(
        const std::string& win_name,
        bool inheritable = false,
        const NamedPipeConfig& config = {});

    [[nodiscard]] std::optional<u32> pipe_file_mode(Handle handle) const;

    /// WaitNamedPipeA/W backing. Returns true when a server instance exists for
    /// @p win_name. Per Windows, if no instances exist the call fails
    /// immediately (ERROR_FILE_NOT_FOUND) regardless of timeout.
    [[nodiscard]] bool wait_named_pipe(const std::string& win_name,
                                       u32 timeout_ms);

    /// Serialize inheritable pipe *and* file handles for a CreateProcess
    /// relaunch. File entries use an `F,` descriptor so an inherited
    /// log-file handle (PROC_THREAD_ATTRIBUTE_HANDLE_LIST) survives.
    [[nodiscard]] std::string prepare_inherited_named_pipes();

    /// Serialize only the inheritable pipe/file handles selected by a
    /// PROC_THREAD_ATTRIBUTE_HANDLE_LIST. Internal console attachment can
    /// transfer its channels independently of ordinary handle inheritance.
    [[nodiscard]] std::string prepare_inherited_named_pipes(
        std::span<const Handle> selected_handles, bool require_inheritable = true);

    /// Import listeners/files serialized by prepare_inherited_named_pipes().
    [[nodiscard]] bool import_inherited_named_pipes(std::string_view descriptors);

    /// Runtime DuplicateHandle INTO another process: serialize pipe or file
    /// metadata and return the host FD that must be sent with SCM_RIGHTS.
    /// Microsoft Learn lists Pipe and File as DuplicateHandle-supported types.
    [[nodiscard]] std::optional<std::string> prepare_transferred_handle(
        Handle source, u32 access, bool inheritable, int& out_fd);

    /// Import a runtime-transferred pipe or file using @p transferred_fd from
    /// SCM_RIGHTS.
    [[nodiscard]] Handle import_transferred_handle(std::string_view descriptor,
                                                   int transferred_fd);

    /// Same-process DuplicateHandle for Pipe or File.
    [[nodiscard]] Handle duplicate_handle(Handle source, u32 flags, u32 access);

    void restore_inherited_pipe_close_on_exec();

    /// SetNamedPipeHandleState backing. Applies Windows-visible pipe state to
    /// @p handle. Pointer arguments are optional and read only when non-null.
    /// @param error receives the Win32 error to report when this returns false.
    /// @return false unless the requested state was actually accepted.
    [[nodiscard]] bool set_named_pipe_handle_state(
        Handle handle,
        const u32* mode,
        const u32* max_collection_count,
        const u32* collect_data_timeout,
        u32& error);

    /// GetNamedPipeHandleState backing. Reports the per-HANDLE state a caller
    /// set through SetNamedPipeHandleState, as opposed to named_pipe_info's
    /// per-PIPE configuration.
    /// @return false (with @p error set) for an invalid or non-pipe handle.
    [[nodiscard]] bool named_pipe_handle_state(Handle handle,
                                               u32* state,
                                               u32* current_instances,
                                               u32* max_collection_count,
                                               u32* collect_data_timeout,
                                               u32& error);

    /// GetNamedPipeInfo backing. Fills the non-null outputs with the endpoint's
    /// Windows-visible configuration.
    /// @return false (with @p error set) for an invalid or non-pipe handle.
    [[nodiscard]] bool named_pipe_info(Handle handle,
                                       u32* flags,
                                       u32* out_buffer_size,
                                       u32* in_buffer_size,
                                       u32* max_instances,
                                       u32& error);

    [[nodiscard]] bool create_anonymous_pipe(Handle& read_handle,
                                             Handle& write_handle,
                                             bool inheritable);

    /// Create a process-local named-pipe endpoint when cross-process IPC is
    /// unavailable. The returned handle has normal HandleTable lifetime.
    [[nodiscard]] Handle create_local_named_pipe(
        std::string win_name = std::string{});

    /// Return the Windows FileNameInfo name for a named-pipe handle.
    ///
    /// Windows' named-pipe filesystem reports the name relative to
    /// "\\.\pipe", for example "\pipe_1234_ABC". A client queries this on an
    /// inherited first pipe instance to recover the name that new clients
    /// should open.
    [[nodiscard]] std::optional<std::string> named_pipe_file_name(
        Handle h) const;

    /// Accept one client connection on a named-pipe server handle.
    /// @param timeout_ms <0 blocks, 0 polls.
    [[nodiscard]] PipeConnectResult connect_named_pipe(
        Handle h, int timeout_ms);

    /// Mark a disconnected server pipe as having an asynchronous connect in
    /// progress. Pending means the operation was successfully armed.
    /// @p generation receives the armed request's number, which
    /// wait_async_named_pipe_connect waits on.
    [[nodiscard]] PipeConnectResult begin_async_named_pipe_connect(
        Handle h, GuestAddr overlapped = 0, u64* generation = nullptr);

    /// Wait cooperatively for the client of an armed asynchronous connect.
    /// Cancel @p ep's pending overlapped ConnectNamedPipe (all when
    /// @p overlapped is 0; only the caller's when @p only_calling_thread).
    /// Returns the number cancelled (0 or 1).
    usize cancel_named_pipe_connect(const std::shared_ptr<PipeEndpoint>& ep,
                                    Handle issued_on, GuestAddr overlapped,
                                    bool only_calling_thread, bool by_close = false);
    [[nodiscard]] PipeConnectResult wait_async_named_pipe_connect(
        Handle h, u64 generation, std::stop_token stop_token);

    /// Disconnect the accepted client from a server pipe instance so a
    /// subsequent ConnectNamedPipe can accept the next client.
    [[nodiscard]] PipeDisconnectResult disconnect_named_pipe(Handle h);

    /// GetNamedPipeClientProcessId — server end only, after a client connect.
    /// Peer identity comes from AF_UNIX LOCAL_PEERPID (Vitreo guest PID ==
    /// host getpid()).
    [[nodiscard]] bool named_pipe_client_process_id(
        Handle h, u32& out_pid, u32& error);

    /// GetNamedPipeServerProcessId — CreateNamedPipe handle returns the
    /// creating process id; a connected CreateFile client handle returns the
    /// peer server pid. Accept uses that id to open the remote process.
    [[nodiscard]] bool named_pipe_server_process_id(
        Handle h, u32& out_pid, u32& error);

    /// Close a file handle previously opened via create_file
    /// @param h Win32 handle to close
    /// @return true if the handle was valid and closed successfully
    bool close_file(Handle h);

    /// HandleTable close-callback hook.
    ///
    /// This is invoked by HandleTable::close (CloseHandle) so FileManager can
    /// release any VFS resources associated with file handles and clean up
    /// pipe endpoints.
    ///
    /// @param h The handle value being closed
    /// @param type The handle type
    /// @param object The handle's object pointer (type-specific)
    /// @return true if cleanup succeeded or no cleanup needed
    bool on_handle_close(Handle h, HandleType type, void* object) noexcept;

    // -------------------------------------------------------------------------
    // ReadFile / WriteFile / SetFilePointer / GetFileSize  (WIN32-011)
    // -------------------------------------------------------------------------

    /// ReadFile – read data from file into guest memory buffer
    /// @param handle Win32 file handle
    /// @param buffer_addr Guest address of buffer to read into
    /// @param bytes_to_read Number of bytes to read
    /// @param bytes_read_addr Guest address to write number of bytes actually read (u32)
    /// @return true on success
    bool read_file(Handle handle, GuestAddr buffer_addr, u32 bytes_to_read,
                   GuestAddr bytes_read_addr, u32* bytes_read_out = nullptr);

    /// Positioned read used by OVERLAPPED I/O.
    /// @param advance_pointer When true (synchronous handle + lpOverlapped),
    ///   the file pointer moves to Offset+bytes on success. FILE_FLAG_OVERLAPPED
    ///   handles leave the pointer unchanged (files_core_probe async_ReadFile_*).
    bool read_file_at(Handle handle, GuestAddr buffer_addr, u32 bytes_to_read,
                      GuestAddr bytes_read_addr, u64 offset,
                      u32* bytes_read_out = nullptr,
                      bool advance_pointer = true);

    /// True when CreateFile opened this file with FILE_FLAG_OVERLAPPED.
    [[nodiscard]] bool is_overlapped_file(Handle handle) const;

    /// Raw volume ReadFile for CreateFile("\\\\.\\X:") Device handles.
    bool read_volume_at(Handle handle, GuestAddr buffer_addr, u32 bytes_to_read,
                        GuestAddr bytes_read_addr, u64 offset,
                        u32* bytes_read_out = nullptr);

    /// WriteFile – write data from guest memory buffer to file
    /// @param handle Win32 file handle
    /// @param buffer_addr Guest address of buffer containing data
    /// @param bytes_to_write Number of bytes to write
    /// @param bytes_written_addr Guest address to write number of bytes actually written (u32)
    /// @return true on success
    /// Write host bytes to a pipe handle. write_file sources its payload from
    /// GUEST memory; ConPTY has host bytes (VT the console generated) and no
    /// guest buffer to point at.
    [[nodiscard]] bool write_handle_bytes(Handle handle, std::string_view bytes);

    /// Host-side file read for TransmitFile / TransmitPackets. Reads up to
    /// `max_bytes` (0 = rest of file) from `absolute_offset` if set, otherwise
    /// from the current file pointer. The file pointer is restored afterward
    /// (Windows TransmitFile does not advance it — ws2_transmit_probe).
    [[nodiscard]] bool read_handle_bytes(
        Handle handle, std::vector<u8>& out, u32 max_bytes,
        std::optional<u64> absolute_offset = std::nullopt);

    bool write_file(Handle handle, GuestAddr buffer_addr, u32 bytes_to_write,
                    GuestAddr bytes_written_addr, u32* bytes_written_out = nullptr);

    /// Positioned write used by OVERLAPPED I/O.
    /// @param advance_pointer When true (synchronous handle + lpOverlapped),
    ///   the file pointer moves to Offset+bytes on success. FILE_FLAG_OVERLAPPED
    ///   handles leave it unchanged (async_writefile_probe pointer_after).
    bool write_file_at(Handle handle, GuestAddr buffer_addr, u32 bytes_to_write,
                       GuestAddr bytes_written_addr, u64 offset,
                       u32* bytes_written_out = nullptr,
                       bool advance_pointer = true);

    /// TransactNamedPipe - write one request and synchronously read its reply.
    bool transact_named_pipe(Handle handle, GuestAddr input_addr, u32 input_size,
                             GuestAddr output_addr, u32 output_size,
                             GuestAddr bytes_read_addr);

    /// SetFilePointer – move file pointer
    /// @return New low-32 position, or 0xFFFFFFFF on failure
    u32 set_file_pointer(Handle handle, i32 distance, i32* distance_high,
                         u32 move_method);

    /// Unambiguous 64-bit seek: the new position, or nullopt on failure. A
    /// position whose low half is 0xFFFFFFFF is a success, not the sentinel.
    [[nodiscard]] std::optional<u64> set_file_pointer64(Handle handle,
                                                        i64 offset,
                                                        u32 move_method);

    /// GetFileSize – get size of file
    /// @return Low-32 file size, or 0xFFFFFFFF on failure
    u32 get_file_size(Handle handle, u32* high_part);

    /// Unambiguous 64-bit file size query used by BOOL-returning frontends.
    /// Unlike GetFileSize's DWORD result, a valid 0xFFFFFFFF-byte file is not
    /// confused with failure.
    [[nodiscard]] std::optional<u64> get_file_size64(Handle handle);

    /// SetEndOfFile – truncate or extend the file at its current position.
    [[nodiscard]] bool set_end_of_file(Handle handle);

    // -------------------------------------------------------------------------
    // NT-style File I/O (for NtReadFile/NtWriteFile)
    // -------------------------------------------------------------------------

    /// NtReadFile – read data from file with optional offset
    /// @param handle Win32 file handle
    /// @param buffer_addr Guest address of buffer to read into
    /// @param length Number of bytes to read
    /// @param byte_offset File offset (negative = use current position)
    /// @param bytes_read Output: number of bytes actually read
    /// @return NTSTATUS (STATUS_SUCCESS, STATUS_END_OF_FILE, STATUS_ACCESS_DENIED, etc.)
    u32 nt_read_file(Handle handle, GuestAddr buffer_addr, u32 length,
                     i64 byte_offset, u32& bytes_read);

    /// NtWriteFile – write data to file with optional offset
    /// @param handle Win32 file handle
    /// @param buffer_addr Guest address of buffer containing data
    /// @param length Number of bytes to write
    /// @param byte_offset File offset (negative = use current position)
    /// @param bytes_written Output: number of bytes actually written
    /// @return NTSTATUS (STATUS_SUCCESS, STATUS_ACCESS_DENIED, etc.)
    u32 nt_write_file(Handle handle, GuestAddr buffer_addr, u32 length,
                      i64 byte_offset, u32& bytes_written);

    // -------------------------------------------------------------------------
    // Win32 → VFS Conversion Helpers
    // -------------------------------------------------------------------------

    /// Map Win32 dwDesiredAccess flags to VFS AccessMode
    [[nodiscard]] static capsule::AccessMode to_access_mode(u32 desired_access) noexcept;

    /// Map Win32 dwShareMode flags to VFS ShareMode
    [[nodiscard]] static capsule::ShareMode to_share_mode(u32 share_mode) noexcept;

    /// Map Win32 dwCreationDisposition value to VFS CreationDisposition
    /// @return CreationDisposition enum, defaults to OpenExisting for unknown values
    [[nodiscard]] static capsule::CreationDisposition to_disposition(u32 creation_disp) noexcept;

private:
    capsule::Vfs& m_vfs;           ///< VFS subsystem
    HandleTable& m_handles;         ///< Win32 handle table
    GuestMemory* m_memory = nullptr; ///< Guest memory (optional, for ReadFile/WriteFile)
    ConsoleManager* m_console = nullptr; ///< Console bridge (WIN32-089)
    FileOpsManager* m_file_ops = nullptr;
    FileMappingManager* m_file_mapping_manager = nullptr;
    OverlappedCompletionCallback m_overlapped_completion;
    OverlappedTraceCallback m_overlapped_trace;
    SocketHandleProbe m_socket_handle_probe;
    IoCompletionRoutineQueue m_io_completion_routine_queue;
    std::mutex m_positioned_io_mutex;
    mutable std::mutex m_device_mutex;
    std::mutex m_overlapped_pipe_io_mutex;
    /// Pipe transports whose FD_CLOEXEC prepare_inherited_named_pipes cleared
    /// for the next spawn; any of them left inheritable would leak into every
    /// later child and keep its peer from ever observing the close.
    std::mutex m_close_on_exec_cleared_mutex;
    std::vector<std::shared_ptr<ipc::UnixPipe>> m_close_on_exec_cleared;
    std::condition_variable m_overlapped_pipe_completion_cv;
    std::mutex m_sync_io_mutex;
    std::unordered_map<u32, SyncIoCancelFlag> m_sync_io_by_tid;
    std::jthread m_overlapped_pipe_completion_worker;
    std::jthread m_overlapped_pipe_read_worker;
    /// A pending overlapped pipe read: @p run returns true once it has
    /// completed (or aborted) the operation; @p fd is the descriptor whose
    /// readability can complete it, so the worker can sleep in poll() on all
    /// of them instead of re-running every task each millisecond.
    struct PipeReadTask {
        int fd{-1};
        std::function<bool(std::stop_token)> run;
    };
    std::vector<PipeReadTask> m_pending_pipe_read_tasks;
    /// Self-pipe that wakes the read worker out of poll(): a new task, a
    /// cancellation, or shutdown.
    int m_pipe_read_wake[2]{-1, -1};
    void wake_pipe_read_worker() noexcept;

    struct PendingPipeIo {
        Handle handle{0};
        GuestAddr overlapped{0};
        u32 issuer_tid{0};
        bool is_64bit{false};
        bool cancelled{false};
    };
    std::vector<PendingPipeIo> m_pending_pipe_io;

    struct DelayedPipeCompletion {
        Handle handle{0};
        u32 bytes_transferred{0};
        GuestAddr overlapped{0};
        bool is_64bit{false};
        u32 nt_status{0};
        u32 win_error{0};
        std::chrono::steady_clock::time_point ready_at{};
    };
    std::vector<DelayedPipeCompletion> m_delayed_pipe_completions;

    void track_pending_pipe_io(
        Handle handle, GuestAddr overlapped, bool is_64bit);
    /// Remove tracking for `overlapped`. Returns nullopt if not found;
    /// otherwise whether the op was marked cancelled.
    [[nodiscard]] std::optional<bool> consume_pending_pipe_io(
        GuestAddr overlapped);
    void notify_pipe_io_aborted(
        Handle handle, GuestAddr overlapped, bool is_64bit) const;
    /// Caller holds `m_overlapped_pipe_io_mutex`.
    void enqueue_delayed_pipe_completion(
        Handle handle, u32 bytes_transferred, GuestAddr overlapped,
        bool is_64bit, u32 nt_status = 0, u32 win_error = 0);
    void run_overlapped_pipe_completion_worker(std::stop_token stop);
    void run_overlapped_pipe_read_worker(std::stop_token stop);
    void deliver_delayed_pipe_completion(
        const DelayedPipeCompletion& completion, bool force_abort);

    /// Pended overlapped pipe writes are carried by one kqueue service
    /// thread, not one thread per operation. An endpoint is listed here
    /// (guarded by m_overlapped_pipe_io_mutex) while its queued_writes is
    /// non-empty, and its descriptor is armed EVFILT_WRITE | EV_ONESHOT; the
    /// service drains that endpoint's FIFO when the peer frees buffer space.
    struct ArmedPipeWriter {
        std::shared_ptr<PipeEndpoint> endpoint;
        std::shared_ptr<ipc::UnixPipe> io;
    };
    std::vector<ArmedPipeWriter> m_armed_pipe_writers;
    int m_pipe_write_kq{-1};
    std::jthread m_pipe_write_service;
    /// Caller holds m_overlapped_pipe_io_mutex. False if no kqueue exists.
    [[nodiscard]] bool ensure_pipe_write_service();
    [[nodiscard]] bool arm_pipe_writer(int fd) const noexcept;
    void wake_pipe_write_service() const noexcept;
    void run_pipe_write_service(std::stop_token stop);
    /// Send what the transport accepts from @p writer's queue in FIFO order,
    /// abort cancelled requests (all of them when @p abort_all), and deliver
    /// the resulting completions after releasing every lock.
    void drain_pipe_writer(const ArmedPipeWriter& writer, bool abort_all);

    /// Serializes CreateFile/CloseHandle/DuplicateHandle updates to the
    /// path-keyed maps below. Concurrent CreateFile was racing
    /// `m_path_open_counts` emplace (host SIGSEGV under concurrent open).
    mutable std::mutex m_file_path_mutex;

    /// Tracks Win32 Handle → VFS FileHandle for cleanup
    std::unordered_map<Handle, capsule::FileHandle> m_vfs_handles;

    /// Handles from adopt_vfs_handle(owns_vfs=false): closing one leaves its
    /// VFS file open for the owner that still holds it.
    std::set<Handle> m_unowned_vfs_handles;

    /// Guest path each file handle was opened on. Windows resolves a handle
    /// back to its name for delete-on-close; the VFS deletes by path, so the
    /// name has to be retained for the lifetime of the handle.
    std::unordered_map<Handle, std::string> m_handle_paths;

    /// Handles granted FILE_APPEND_DATA without FILE_WRITE_DATA. Windows sends
    /// every write on such a handle to the current end of file and ignores the
    /// file pointer, which is what lets several processes append to one file
    /// without overwriting each other. The VFS has only a per-handle position,
    /// so the append has to be re-resolved on each write.
    std::set<Handle> m_append_only_handles;

    /// File handles opened with FILE_FLAG_OVERLAPPED (CreateFile #7 / ReadFile #2).
    std::set<Handle> m_overlapped_file_handles;

    /// File handles opened with FILE_FLAG_NO_BUFFERING (SetFilePointer #4).
    std::set<Handle> m_no_buffering_file_handles;

    /// Whether writes on this handle must go to the current end of file.
    bool is_append_only(Handle handle) const;

    [[nodiscard]] bool is_no_buffering_file(Handle handle) const;

    /// Delete disposition is a property of the *object*, not of one handle:
    /// Windows removes it when the last handle closes, and refuses new opens
    /// in the meantime. Both maps are keyed by the path each handle was
    /// opened on.
    std::map<std::string, u32> m_path_open_counts;
    std::set<std::string> m_paths_delete_pending;
    /// Handles opened with FILE_FLAG_DELETE_ON_CLOSE: closing one of them
    /// marks its file delete-pending (Learn: FILE_FLAG_DELETE_ON_CLOSE).
    std::set<Handle> m_delete_on_close_handles;
    std::set<std::string> m_running_kernel_devices;

    void trace_overlapped(OverlappedIoTraceEvent event) const;
};

// =============================================================================
// Thunk Implementations
// =============================================================================

/// CreateFileW thunk implementation
///
/// HANDLE WINAPI CreateFileW(
///     LPCWSTR lpFileName,
///     DWORD dwDesiredAccess,
///     DWORD dwShareMode,
///     LPSECURITY_ATTRIBUTES lpSecurityAttributes,  // ignored
///     DWORD dwCreationDisposition,
///     DWORD dwFlagsAndAttributes,
///     HANDLE hTemplateFile  // ignored
/// );
void impl_CreateFileW(Marshaler& m, FileManager& fm);

/// CreateFileA thunk implementation
///
/// HANDLE WINAPI CreateFileA(
///     LPCSTR lpFileName,
///     DWORD dwDesiredAccess,
///     DWORD dwShareMode,
///     LPSECURITY_ATTRIBUTES lpSecurityAttributes,  // ignored
///     DWORD dwCreationDisposition,
///     DWORD dwFlagsAndAttributes,
///     HANDLE hTemplateFile  // ignored
/// );
void impl_CreateFileA(Marshaler& m, FileManager& fm);

/// ReOpenFile thunk — HANDLE ReOpenFile(HANDLE, DWORD, DWORD, DWORD).
void impl_ReOpenFile(Marshaler& m, FileManager& fm);

/// WriteFile thunk implementation
///
/// BOOL WINAPI WriteFile(
///     HANDLE hFile,           // RCX
///     LPCVOID lpBuffer,       // RDX
///     DWORD nNumberOfBytesToWrite, // R8
///     LPDWORD lpNumberOfBytesWritten, // R9
///     LPOVERLAPPED lpOverlapped
/// );
void impl_WriteFile(Marshaler& m, FileManager& fm);

/// ReadFile thunk implementation
///
/// BOOL WINAPI ReadFile(
///     HANDLE hFile,           // RCX
///     LPVOID lpBuffer,        // RDX
///     DWORD nNumberOfBytesToRead, // R8
///     LPDWORD lpNumberOfBytesRead, // R9
///     LPOVERLAPPED lpOverlapped
/// );
void impl_ReadFile(Marshaler& m, FileManager& fm);

/// ReadFileEx thunk — overlapped read + FileIOCompletionRoutine APC.
void impl_ReadFileEx(Marshaler& m, FileManager& fm);

/// WriteFileEx thunk — overlapped write + FileIOCompletionRoutine APC.
void impl_WriteFileEx(Marshaler& m, FileManager& fm);

/// Create DllExports entries for all file I/O functions
/// @param fm Reference to the FileManager (captured by thunk lambdas)
/// @return DllExports containing file API entries
DllExports make_file_exports(FileManager& fm);

/// Map a VFS error onto its Win32 error code.
///
/// Single source of truth for every kernel32 file API. Notably it keeps
/// PathNotFound (a missing directory, ERROR_PATH_NOT_FOUND) distinct from
/// FileNotFound (a missing leaf, ERROR_FILE_NOT_FOUND) — applications branch
/// on that difference.
[[nodiscard]] u32 vfs_error_to_win32(capsule::VfsError err) noexcept;

} // namespace vitreo::winapi
