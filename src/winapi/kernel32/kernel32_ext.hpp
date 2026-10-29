// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file kernel32_ext.hpp
/// @brief Extended kernel32 API stubs for common Win32 applications
///
/// Provides SetErrorMode, CompareFileTime, CopyFileW, CreateProcessW,
/// ExpandEnvironmentStringsW, GetDiskFreeSpaceW, GetExitCodeProcess,
/// GetFullPathNameW, GetPrivateProfileStringW, GetShortPathNameW,
/// GetTempFileNameW, GetTempPathW, GetWindowsDirectoryW, MoveFileW,
/// MulDiv, RemoveDirectoryW, SearchPathW, SetCurrentDirectoryW,
/// WritePrivateProfileStringW, lstrcmpW, lstrcmpiA, lstrcmpiW,
/// lstrcpyA, lstrcpynW, lstrlenA.

#include <vitreo/common.hpp>
#include "winapi/common/elevation.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/handle.hpp"
#include "core/memory/guest_memory.hpp"
#include "ipc/process_registry.hpp"
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <atomic>
#include <functional>
#include <optional>
#include <thread>
#include <vector>

namespace vitreo::capsule {
class Vfs;
}

namespace vitreo::ipc {
class ProcessSession;
struct ProcessObject;
}

namespace vitreo::platform {
class ElevationAuthorizer;
}

namespace vitreo::cpu { struct CpuState; }

namespace vitreo::winapi {

/// The NT device an MS-DOS drive name links to (QueryDosDevice's target), or
/// nullopt for a letter with no drive: C: is \Device\HarddiskVolume3, the
/// other fixed drives follow in letter order, optical drives are
/// \Device\CdRom0, 1, ... (query_dos_device_probe).
[[nodiscard]] std::optional<std::string> nt_device_for_drive(capsule::Vfs& vfs,
                                                             char letter);
/// "C:\dir\file" as the NT device path the same drive reports
/// ("\Device\HarddiskVolume3\dir\file"), or nullopt without a drive letter.
/// Every API that returns device paths uses this, so a caller can map them
/// back with QueryDosDevice.
[[nodiscard]] std::optional<std::string> win32_to_nt_device_path(
    capsule::Vfs& vfs, std::string_view path);

/// A system executable the runtime implements internally rather than loading
/// as a PE from the capsule. CreateProcess resolves these by image name
/// before touching the filesystem, so a guest can launch one exactly as it
/// would the real thing.
///
/// The handler receives the full command line and returns the process exit
/// code. It runs synchronously: the caller gets back a handle to a process
/// that has already finished, which is all a parent doing
/// CreateProcess/WaitForSingleObject/GetExitCodeProcess can observe.
///
/// This registry exists so that kernel32 need not know what any particular
/// builtin *is* — the owning subsystem registers itself.
using BuiltinProcessHandler = std::function<u32(const std::string& command_line)>;

/// Register @p handler for an image name such as "msiexec.exe". Matching is
/// case-insensitive and on the file name only, since a guest may launch it
/// bare, by full path, or from any working directory.
void register_builtin_process(const std::string& image_name,
                              BuiltinProcessHandler handler);

/// Look up a builtin by image path or bare name; nullptr when there is none.
[[nodiscard]] const BuiltinProcessHandler* find_builtin_process(
    const std::string& image_path);

class GlobalLocalManager;

class EnvironmentManager;
class FileMappingManager;
class FileManager;
struct OverlappedIoTraceEvent;
enum class OverlappedIoTraceStage : u8;
class FileSearchManager;
class Marshaler;
class ModuleManager;
class ConsoleManager;
class ProcessManager;
class SecurityStubs;
class NtdllExtManager;
class SysInfoManager;
class SyncManager;
class CriticalSectionManager;
class ThreadManager;
class DirectoryChangeService;
class VirtualMemoryManager;

// =============================================================================
// Kernel32ExtManager
// =============================================================================

/// Extended kernel32 API surface.
///
/// String and comparison operations work directly on guest memory.
class Kernel32ExtManager {
public:
    using GuestCall = std::function<u64(GuestAddr, const u64*, usize, bool)>;
    using ProcessSnapshotProvider =
        std::function<std::vector<ipc::ProcessSnapshotEntry>()>;

    /// Construct with guest memory reference
    /// @param memory Reference to guest memory for read/write operations
    Kernel32ExtManager(GuestMemory& memory, capsule::Vfs& vfs);

    /// No copy
    Kernel32ExtManager(const Kernel32ExtManager&) = delete;
    Kernel32ExtManager& operator=(const Kernel32ExtManager&) = delete;

    /// No move (contains reference)
    Kernel32ExtManager(Kernel32ExtManager&&) = delete;
    Kernel32ExtManager& operator=(Kernel32ExtManager&&) = delete;

    /// Stops and joins the job member-exit watchers.
    ~Kernel32ExtManager();

    /// Set the current directory (for GetCurrentDirectoryW and path resolution)
    void set_current_directory(const std::string& dir);
    /// SetCurrentDirectoryA/W body: validate, normalize, then switch.
    [[nodiscard]] bool change_current_directory(const std::string& directory);
    /// RemoveDirectoryA/W body.
    void remove_directory_common(Marshaler& m, bool wide);

    /// Interrupt internal I/O-completion waits after the guest process exits.
    /// This does not enqueue, remove, or complete any guest-visible packet.
    void begin_shutdown();

    /// Access the VFS (for legacy file API thunks)
    capsule::Vfs& vfs() noexcept { return m_vfs; }

    /// Access guest memory
    GuestMemory& memory() noexcept { return m_memory; }

    /// Read null-terminated ANSI string from guest memory
    u32 guest_strlen(GuestAddr addr);

    // -------------------------------------------------------------------------
    // Error Handling
    // -------------------------------------------------------------------------

    /// SetErrorMode — store new process mode, return previous
    void impl_SetErrorMode(Marshaler& m);

    /// GetErrorMode — the process error mode
    void impl_GetErrorMode(Marshaler& m);

    /// GetThreadErrorMode — the calling thread's error mode
    void impl_GetThreadErrorMode(Marshaler& m);

    /// Effective error mode for the calling thread: its SetThreadErrorMode
    /// override if it has one, otherwise the process error mode.
    [[nodiscard]] u32 current_thread_error_mode() const;

    // -------------------------------------------------------------------------
    // File Time
    // -------------------------------------------------------------------------

    /// CompareFileTime — compare two FILETIME structs as u64
    void impl_CompareFileTime(Marshaler& m);

    // -------------------------------------------------------------------------
    // File Operations (stubs)
    // -------------------------------------------------------------------------

    /// CopyFileA/W, CopyFileExA/W and CopyFile2 share one engine
    /// (copy_file_engine.cpp, measured by s4_copyfile_probe).
    void impl_CopyFileA(Marshaler& m);
    void impl_CopyFileW(Marshaler& m);
    void impl_CopyFileExA(Marshaler& m);
    void impl_CopyFileExW(Marshaler& m);
    void impl_CopyFile2(Marshaler& m);

    enum class CopyFileApi { CopyFile, CopyFileEx, CopyFile2 };
    struct CopyFileRequest {
        CopyFileApi api = CopyFileApi::CopyFile;
        std::string src;
        std::string dst;
        u32 flags = 0;          ///< dwCopyFlags
        GuestAddr routine = 0;  ///< LPPROGRESS_ROUTINE or PCOPYFILE2_PROGRESS_ROUTINE
        u64 context = 0;        ///< lpData / pvCallbackContext
        GuestAddr cancel = 0;   ///< pbCancel / pfCancel
    };
    /// Run one copy. Returns the Win32 error (0 on success); the caller turns
    /// it into BOOL + last error or an HRESULT.
    [[nodiscard]] u32 run_copy_file(Marshaler& m, const CopyFileRequest& r);

    /// MoveFileW — stub, return TRUE
    void impl_MoveFileW(Marshaler& m);

    /// MoveFileA — ANSI variant of MoveFileW (VFS rename)
    void impl_MoveFileA(Marshaler& m);

    /// RemoveDirectoryW — stub, return TRUE
    void impl_RemoveDirectoryW(Marshaler& m);

    // -------------------------------------------------------------------------
    // Process
    // -------------------------------------------------------------------------

    /// CreateProcessW — launch a new guest worker in this process session.
    void impl_CreateProcessW(Marshaler& m);
    /// CreateProcessAsUserW — validate a primary token, then use the same
    /// process-creation contract with that token's modeled security state.
    void impl_CreateProcessAsUserW(Marshaler& m);
    /// CreateProcessWithTokenW — Learn: create a process in the security
    /// context of @p hToken. Arg layout differs from CreateProcessAsUserW
    /// (no SECURITY_ATTRIBUTES / bInheritHandles; dwLogonFlags after token).
    void impl_CreateProcessWithTokenW(Marshaler& m);
    /// CreateProcessWithLogonW: authenticates against the capsule's one
    /// account, then creates the process like CreateProcessWithTokenW.
    void impl_CreateProcessWithLogonW(Marshaler& m);

    /// Launch an executable for ShellExecuteW/ExW without terminating the
    /// calling guest process. Returns false when the executable cannot be
    /// resolved or the child Vitreo process cannot be started.
    /// ShellExecuteEx launch: `directory` is lpDirectory (empty = inherit the
    /// caller's), `show_cmd` is nShow, handed to the child as STARTUPINFO
    /// STARTF_USESHOWWINDOW / wShowWindow.
    [[nodiscard]] bool launch_shell_executable(
        const std::string& app_path,
        const std::string& parameters,
        const std::string& directory,
        i32 show_cmd,
        bool request_elevation,
        Handle& process_handle,
        u32& error);

    /// Learn CreateProcess: a new process's primary thread starts suspended
    /// only when its creator passes CREATE_SUSPENDED. Adds
    /// VITREO_PRIMARY_THREAD_SUSPENDED=0 to a launch environment that does
    /// not state it, so a child never inherits its parent's value (a
    /// suspended-created launcher's ShellExecute child otherwise waits for a
    /// ResumeThread nobody sends).
    static void state_primary_thread_suspension(
        std::vector<std::string>& extra_env);

    /// The calling process's environment as "name=value" entries, names
    /// spelled as set (the block is case-preserving): what a child started
    /// without an explicit environment inherits (CreateProcess with a NULL
    /// lpEnvironment, ShellExecute, LoadModule with a NULL lpEnvAddress).
    [[nodiscard]] std::vector<std::string> inherited_guest_environment() const;

    /// GetExitCodeProcess — report STILL_ACTIVE or the tracked child's exit code.
    void impl_GetExitCodeProcess(Marshaler& m);

    // -------------------------------------------------------------------------
    // Environment / Paths
    // -------------------------------------------------------------------------

    /// ExpandEnvironmentStringsA — expand %VAR% tokens into an ANSI buffer
    void impl_ExpandEnvironmentStringsA(Marshaler& m);

    /// ExpandEnvironmentStringsW — expand %VAR% tokens into a UTF-16 buffer
    void impl_ExpandEnvironmentStringsW(Marshaler& m);

    /// GetFullPathNameW — copy input path to output buffer
    void impl_GetFullPathNameW(Marshaler& m);

    /// GetShortPathNameW — copy long path to short path (no conversion)
    void impl_GetShortPathNameW(Marshaler& m);

    /// GetShortPathNameA — ANSI variant of GetShortPathNameW (no 8.3 conversion)
    void impl_GetShortPathNameA(Marshaler& m);

    /// GetTempFileName{W,A}: "<path>\\<pre><hex>.tmp" (Learn GetTempFileNameW;
    /// undocumented rules measured by tools/oracle_probe/gettempfilename_probe).
    void impl_GetTempFileNameW(Marshaler& m);
    /// The outcome both exports share, in UTF-16 units.
    struct TempFileNameResult {
        u32 value = 0;               // return value (0 = failure)
        bool buffer_written = false; // W leaves `buffer` in lpTempFileName
        std::u16string buffer;       // stripped path, or the (last) candidate
    };
    /// Sets the last error exactly as Windows does for each outcome.
    [[nodiscard]] TempFileNameResult temp_file_name(std::u16string path,
                                                    std::u16string_view prefix,
                                                    u32 unique);

    /// GetTempPathW — write "C:\Temp\" to buffer
    void impl_GetTempPathW(Marshaler& m);
    /// GetTempPath{A,W}: TMP, TEMP, USERPROFILE, then the Windows directory,
    /// backslash-terminated (Learn). Creates the directory in the capsule.
    [[nodiscard]] std::string resolve_temp_path();

    /// GetWindowsDirectoryW — write "C:\Windows" to buffer
    void impl_GetWindowsDirectoryW(Marshaler& m);
    /// GetSystemWow64DirectoryW — write the 32-bit WOW64 system directory.
    void impl_GetSystemWow64DirectoryW(Marshaler& m);
    /// GetSystemWow64DirectoryA — ANSI form of GetSystemWow64DirectoryW.
    void impl_GetSystemWow64DirectoryA(Marshaler& m);

    /// SetCurrentDirectoryW — stub, return TRUE
    void impl_SetCurrentDirectoryW(Marshaler& m);

    /// SetCurrentDirectoryA — ANSI variant of SetCurrentDirectoryW
    void impl_SetCurrentDirectoryA(Marshaler& m);

    /// GetCurrentDirectoryW — return last SetCurrentDirectoryW value
    void impl_GetCurrentDirectoryW(Marshaler& m);

    /// SearchPathA/W — search the capsule filesystem and process search path
    void impl_SearchPathA(Marshaler& m);
    void impl_SearchPathW(Marshaler& m);
    /// SetSearchPathMode — per-process SearchPath safe mode
    /// (setsearchpathmode_probe{32,64}.expected).
    void impl_SetSearchPathMode(Marshaler& m);

    // -------------------------------------------------------------------------
    // Disk
    // -------------------------------------------------------------------------

    /// GetDiskFreeSpaceW — write fake disk geometry, return TRUE
    void impl_GetDiskFreeSpaceW(Marshaler& m);

    // -------------------------------------------------------------------------
    // INI / Profile
    // -------------------------------------------------------------------------

    /// GetPrivateProfileStringW — return default value or empty string
    void impl_GetPrivateProfileStringW(Marshaler& m);

    /// WritePrivateProfileStringW — stub, return TRUE
    void impl_WritePrivateProfileStringW(Marshaler& m);

    // -------------------------------------------------------------------------
    // Math
    // -------------------------------------------------------------------------

    /// MulDiv — (nNumber * nNumerator) / nDenominator with 64-bit intermediate
    void impl_MulDiv(Marshaler& m);

    // -------------------------------------------------------------------------
    // String Operations
    // -------------------------------------------------------------------------

    /// lstrcmpW — compare two wide strings char by char
    void impl_lstrcmpW(Marshaler& m);

    /// lstrcmpiA — compare two ANSI strings case-insensitive
    void impl_lstrcmpiA(Marshaler& m);

    /// lstrcmpiW — compare two wide strings case-insensitive
    void impl_lstrcmpiW(Marshaler& m);

    /// lstrcpyA — copy ANSI string in guest memory
    void impl_lstrcpyA(Marshaler& m);

    /// lstrcpynW — copy up to iMaxLength wide chars
    void impl_lstrcpynW(Marshaler& m);

    /// lstrlenA — length of ANSI string in guest memory
    void impl_lstrlenA(Marshaler& m);

    // -------------------------------------------------------------------------
    // Module Manager Accessor
    // -------------------------------------------------------------------------

    /// Set the optional ModuleManager for LoadLibraryExA / GetModuleHandleEx
    void set_module_manager(ModuleManager* mgr);
    /// Needed by CreateProcess to resolve an HPCON passed in
    /// PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE.
    void set_console_manager(ConsoleManager* mgr) { m_console_manager = mgr; }
    /// Toolhelp process enumeration needs the real PID of the running guest
    /// process; a snapshot that omits the caller is not a snapshot Windows
    /// would ever produce.
    void set_process_manager(ProcessManager* mgr) { m_process_manager = mgr; }
    void set_process_snapshot_provider(ProcessSnapshotProvider provider) {
        m_process_snapshot_provider = std::move(provider);
    }
    void set_handle_table(HandleTable* table) { m_handle_table = table; }
    void set_security_stubs(SecurityStubs* stubs) {
        m_security_stubs = stubs;
    }
    void set_process_session(ipc::ProcessSession* session,
                             std::string capsule_name) {
        m_process_session = session;
        m_capsule_name = std::move(capsule_name);
    }
    /// Persist process-visible capsule state immediately before CreateProcess
    /// snapshots and starts a child. This lets a child observe registry writes
    /// completed by its parent before the launch.
    void set_pre_process_launch_callback(std::function<void()> callback) {
        m_pre_process_launch = std::move(callback);
    }
    /// Learn SetForegroundWindow: a process "started by the foreground
    /// process" may set the foreground. @p may_grant says whether this process
    /// can pass that on now; the child gets VITREO_FOREGROUND_GRANT=1 or =0
    /// (never the parent's own inherited value), and @p granted is told the
    /// child's host pid after a granted launch.
    void set_foreground_grant_hooks(std::function<bool()> may_grant,
                                    std::function<void(int)> granted) {
        m_foreground_may_grant = std::move(may_grant);
        m_foreground_granted = std::move(granted);
    }
    void set_elevation_context(
        const GuestElevationState* state,
        platform::ElevationAuthorizer* authorizer) {
        m_elevation = state;
        m_elevation_authorizer = authorizer;
    }

    [[nodiscard]] std::optional<std::string> process_image_path(
        Handle process_handle) const;
    [[nodiscard]] std::optional<u32> resume_external_primary_thread(
        Handle thread_handle);
    void forget_external_primary_thread(Handle thread_handle);
    /// Drop IOCP associations and completion modes when a File/Pipe handle is
    /// closed so recycled handle values cannot deliver stale packets.
    void on_iocp_handle_close(Handle handle);
    /// CloseHandle of a file handle: completes a pending ReadDirectoryChangesW
    /// with STATUS_NOTIFY_CLEANUP. Called before the IOCP association goes, so
    /// that completion still reaches the port.
    void on_directory_watch_close(Handle handle);
    /// Record @p handle's completion port for the request @p overlapped, so
    /// its packet is still delivered if the handle closes first (completion
    /// routing belongs to the request, not the handle).
    void snapshot_completion_route(Handle handle, GuestAddr overlapped);
    /// Tear down a completion port when its HandleTable entry is closed.
    bool on_iocp_port_close(Handle handle);

private:
    struct CopyFileRun;
    void copy_file_ex_entry(Marshaler& m, bool wide);
    void copy_file_entry(Marshaler& m, bool wide);
    [[nodiscard]] u32 copy_file_callback(CopyFileRun& run, u32 kind, u64 chunk,
                                         u64 chunk_size);
    [[nodiscard]] GuestAddr copy_file_scratch_alloc(Marshaler& m, bool& from_vmm);
    void copy_file_scratch_free(Marshaler& m, GuestAddr page, bool from_vmm);

    void write_process_entry(Marshaler& m, GuestAddr entry, bool wide,
                             const ipc::ProcessSnapshotEntry& process);
    bool write_next_process_entry(Marshaler& m, u32 snapshot,
                                  GuestAddr entry, bool wide, bool first);
    bool write_next_module_entry(Marshaler& m, u32 snapshot,
                                 GuestAddr entry, bool wide, bool first);
    bool write_next_thread_entry(Marshaler& m, u32 snapshot, GuestAddr entry,
                                 bool first);
public:
    [[nodiscard]] ModuleManager* module_manager() const noexcept { return m_module_manager; }
    [[nodiscard]] ProcessManager* process_manager() const noexcept { return m_process_manager; }
    void set_sync_manager(SyncManager* mgr) { m_sync_manager = mgr; }
    void set_critical_section_manager(CriticalSectionManager* mgr) {
        m_critical_section_manager = mgr;
    }
    void set_thread_manager(ThreadManager* mgr) { m_thread_manager = mgr; }
    void set_environment_manager(EnvironmentManager* mgr) { m_environment_manager = mgr; }
    void set_file_search_manager(FileSearchManager* mgr) { m_file_search_manager = mgr; }
    void set_ntdll_ext_manager(NtdllExtManager* mgr) { m_ntdll_ext_manager = mgr; }
    /// VerifyVersionInfo compares against the version GetVersionEx reports
    /// to this process (its manifest decides), which SysInfoManager owns.
    void set_sysinfo_manager(SysInfoManager* mgr) { m_sysinfo_manager = mgr; }
    void set_file_manager(FileManager* mgr);
    [[nodiscard]] FileManager* file_manager() const noexcept { return m_file_manager; }
    void set_file_mapping_manager(FileMappingManager* mgr) {
        m_file_mapping_manager = mgr;
    }
    void complete_overlapped_file_io(Handle file_handle, u32 bytes_transferred,
                                     GuestAddr overlapped, bool is_64bit,
                                     u32 nt_status = 0, u32 win_error = 0,
                                     bool completed_synchronously = false);
    /// Deliver an NT device request's completion (NtDeviceIoControlFile and
    /// friends) whose IO_STATUS_BLOCK is already final: signal its own Event,
    /// queue its ApcRoutine on the issuing thread, or post its ApcContext to
    /// the completion port the handle is associated with. A request that
    /// completed synchronously posts nothing when it failed, nor when it
    /// succeeded on a FILE_SKIP_COMPLETION_PORT_ON_SUCCESS handle
    /// (afd_recv_probe iocp_immediate / iocp_skip_on_success).
    void complete_nt_io(Handle file_handle, GuestAddr apc_context, Handle event,
                        GuestAddr apc_routine, GuestAddr io_status_block,
                        u32 issuing_tid, u32 bytes_transferred, u32 nt_status,
                        u32 win_error, bool synchronous);
    void set_virtual_memory_manager(VirtualMemoryManager* mgr) { m_virtual_memory_manager = mgr; }
    void set_global_local_manager(GlobalLocalManager* mgr) { m_global_local_manager = mgr; }
    void set_guest_call(GuestCall call) { m_guest_call = std::move(call); }
    /// The guest-call hook set_guest_call installed (empty before it).
    [[nodiscard]] const GuestCall& guest_call() const noexcept { return m_guest_call; }

    /// Probe whether a HANDLE value is a Vitreo ws2 SOCKET (separate from
    /// HandleTable). Windows treats SOCKET as a kernel HANDLE for IOCP.
    using SocketHandleProbe = std::function<bool(u32)>;
    void set_socket_handle_probe(SocketHandleProbe probe) {
        m_socket_handle_probe = std::move(probe);
    }

    /// Cancel pending overlapped I/O for a SOCKET (wired to Ws2Manager).
    /// Arguments: guest_socket, overlapped (0 = all), only_calling_thread,
    /// calling_tid. Returns how many ops were marked cancelled.
    using SocketIoCancel = std::function<usize(u32, GuestAddr, bool, u32)>;
    void set_socket_io_cancel(SocketIoCancel cancel) {
        m_socket_io_cancel = std::move(cancel);
    }

    /// Cancel pending overlapped I/O on a Device handle whose requests a
    /// provider owns (the HTTP Server API request queue). Same arguments and
    /// result as SocketIoCancel; providers that do not own the handle return 0.
    using DeviceIoCancel = std::function<usize(Handle, GuestAddr, bool, u32)>;
    void set_device_io_cancel(DeviceIoCancel cancel) {
        m_device_io_cancel = std::move(cancel);
    }

    /// Whether CreateIoCompletionPort associated this handle with a port.
    [[nodiscard]] bool has_iocp_association(Handle handle);

    /// Deliver pending QueueUserAPC callbacks for the current guest thread.
    /// Returns true when one or more APCs ran (alertable waits use this for
    /// WAIT_IO_COMPLETION). Requires set_guest_call / ThreadManager.
    [[nodiscard]] bool try_deliver_user_apcs();

    /// Set the PE resource directory info (from run.cpp after PE loading)
    void set_resource_info(GuestAddr image_base, u32 rsrc_rva, u32 rsrc_size) {
        m_rsrc_image_base = image_base;
        m_rsrc_rva = rsrc_rva;
        m_rsrc_size = rsrc_size;
    }

    /// RT_MESSAGETABLE (type 11, name 1) of @p hmodule (NULL = the main
    /// image) for FormatMessage FROM_HMODULE: the LANGIDs it is present in,
    /// in directory order. False when the module is unknown or has no message
    /// table (ERROR_RESOURCE_DATA_NOT_FOUND).
    [[nodiscard]] bool message_table_languages(GuestAddr hmodule,
                                               std::vector<u16>& out) const;
    /// The MESSAGE_RESOURCE_DATA bytes of that table in exactly @p language.
    [[nodiscard]] bool message_table_data(GuestAddr hmodule, u16 language,
                                          std::vector<u8>& out) const;

    // -------------------------------------------------------------------------
    // Encoding / Decoding (CRT critical)
    // -------------------------------------------------------------------------
    void impl_EncodePointer(Marshaler& m);
    void impl_DecodePointer(Marshaler& m);

    // -------------------------------------------------------------------------
    // Code Page / Locale (CRT critical)
    // -------------------------------------------------------------------------
    void impl_IsValidCodePage(Marshaler& m);
    void impl_GetCPInfo(Marshaler& m);
    void impl_GetStringTypeW(Marshaler& m);
    void impl_LCMapStringW(Marshaler& m);

    // -------------------------------------------------------------------------
    // SEH / Exception
    // -------------------------------------------------------------------------
    void impl_RtlUnwind(Marshaler& m);
    /// x86 RtlUnwind body: run and unlink every registration above the target.
    void unwind_seh32_chain(Marshaler& m, u32 target_frame, u32 erec_ptr,
                            u32 return_value);
    void impl_RaiseException(Marshaler& m);

    // -------------------------------------------------------------------------
    // Synchronization Primitives
    // -------------------------------------------------------------------------
    void impl_InitializeSListHead(Marshaler& m);
    void impl_InterlockedPushEntrySList(Marshaler& m);
    void impl_InitializeSRWLock(Marshaler& m);
    void impl_TryAcquireSRWLockExclusive(Marshaler& m);
    void impl_InitOnceBeginInitialize(Marshaler& m);
    void impl_InitOnceComplete(Marshaler& m);
    void impl_CreateEventA(Marshaler& m);
    void impl_WaitForSingleObjectEx(Marshaler& m);
    void impl_SwitchToThread(Marshaler& m);

    // -------------------------------------------------------------------------
    // Thread
    // -------------------------------------------------------------------------
    void impl_SetThreadPriority(Marshaler& m);
    void impl_GetThreadPriority(Marshaler& m);
    void impl_SetThreadAffinityMask(Marshaler& m);
    void impl_SetThreadIdealProcessor(Marshaler& m);
    void impl_OpenThread(Marshaler& m);
    void impl_GetCurrentProcessorNumber(Marshaler& m);
    void impl_GetCurrentThreadStackLimits(Marshaler& m);
    void impl_SetThreadStackGuarantee(Marshaler& m);

    /// Read the calling thread's NT_TIB stack bounds out of its TEB.
    /// @param low  Receives NtTib.StackLimit (lowest address of the stack).
    /// @param high Receives NtTib.StackBase (one past the highest address).
    /// @return false when no TEB is reachable for the calling thread.
    [[nodiscard]] bool current_thread_stack_bounds(Marshaler& m, u64& low,
                                                   u64& high) const;

    // -------------------------------------------------------------------------
    // Process
    // -------------------------------------------------------------------------
    void impl_GetProcessAffinityMask(Marshaler& m);
    /// Write a DWORD_PTR-sized affinity mask to the guest (4 bytes for a
    /// 32-bit guest, 8 for a 64-bit one).
    void write_affinity_mask(Marshaler& m, GuestAddr ptr, u64 mask);
    /// The process affinity mask in the caller's ABI width: the system mask
    /// until SetProcessAffinityMask narrows it.
    [[nodiscard]] u64 process_affinity_mask(const Marshaler& m) const;
    /// A thread's affinity as Windows reports it: its own mask, or the
    /// process's when it has none, inside the process affinity.
    [[nodiscard]] u64 effective_thread_affinity(const Marshaler& m, u64 stored) const;
    void impl_SetProcessAffinityMask(Marshaler& m);
    void impl_ProcessIdToSessionId(Marshaler& m);

    // -------------------------------------------------------------------------
    // Debug Output
    // -------------------------------------------------------------------------
    void impl_OutputDebugStringA(Marshaler& m);
    void impl_OutputDebugStringW(Marshaler& m);
    void impl_DebugBreak(Marshaler& m);

    // -------------------------------------------------------------------------
    // Handle Operations
    // -------------------------------------------------------------------------
    void impl_DuplicateHandle(Marshaler& m);
    void impl_SetHandleInformation(Marshaler& m);
    u32 socket_handle_flags(Handle handle, u32 mask, u32 flags, bool write);
    void impl_GetHandleInformation(Marshaler& m);

    // -------------------------------------------------------------------------
    // File I/O
    // -------------------------------------------------------------------------
    void impl_FlushFileBuffers(Marshaler& m);
    void impl_SetEndOfFile(Marshaler& m);
    void impl_GetFileTime(Marshaler& m);
    void impl_GetFileInformationByHandle(Marshaler& m);
    void impl_FindFirstFileExW(Marshaler& m);
    void impl_DeviceIoControl(Marshaler& m);
    void impl_CreateIoCompletionPort(Marshaler& m);
    void impl_PostQueuedCompletionStatus(Marshaler& m);
    void impl_GetQueuedCompletionStatusEx(Marshaler& m);
    void impl_SetFileCompletionNotificationModes(Marshaler& m);

    // -------------------------------------------------------------------------
    // Resource
    // -------------------------------------------------------------------------
    void impl_FindResourceA(Marshaler& m);
    void impl_FindResourceExA(Marshaler& m);
    void impl_FindResourceW(Marshaler& m);
    /// Shared ANSI resource lookup used by FindResourceA and FindResourceExA.
    /// Returns an HRSRC handle (0 on failure, with LastError set).
    /// HMODULE / LPCSTR args are full guest pointers — truncating to 32 bits
    /// breaks x64 modules loaded above 4 GiB.
    /// @p language is the FindResourceEx LANGID preference; the plain
    /// FindResource entry points keep the 0x0409 default.
    u32 find_resource_ansi(Marshaler& m, GuestAddr hModule, GuestAddr lpName,
                           GuestAddr lpType, u16 language = 0x0409);
    /// Shared wide resource lookup used by FindResourceW and FindResourceExW.
    u32 find_resource_wide(Marshaler& m, GuestAddr hModule, GuestAddr lpName,
                           GuestAddr lpType, u16 language = 0x0409);
    void impl_LoadResource(Marshaler& m);
    void impl_LockResource(Marshaler& m);
    void impl_SizeofResource(Marshaler& m);
    void impl_LoadStringW(Marshaler& m);

    /// GetPrivateProfileIntA/W — read integer from INI section/key
    void impl_GetPrivateProfileIntA(Marshaler& m);
    void impl_GetPrivateProfileIntW(Marshaler& m);

    /// Shared INI integer lookup used by GetPrivateProfileIntA/W.
    [[nodiscard]] u32 lookup_profile_int(const std::string& app_name,
                                         const std::string& key_name,
                                         u32 n_default,
                                         const std::string& file_name);

    // -------------------------------------------------------------------------
    // Module Loading
    // -------------------------------------------------------------------------
    void impl_LoadLibraryExA(Marshaler& m);
    void get_module_handle_ex_common(Marshaler& m, bool wide);
    void impl_GetModuleHandleExA(Marshaler& m);
    void impl_GetModuleHandleExW(Marshaler& m);

    // Activation contexts (CreateActCtx / ActivateActCtx). HANDLE lifetime
    // and activate/deactivate pairing are tracked; SxS assembly probing is
    // not yet redirected through the active context.
    void impl_CreateActCtxA(Marshaler& m);
    void impl_CreateActCtxW(Marshaler& m);
    void impl_ActivateActCtx(Marshaler& m);
    void impl_DeactivateActCtx(Marshaler& m);
    void impl_ReleaseActCtx(Marshaler& m);
    void impl_GetCurrentActCtx(Marshaler& m);
    void create_actctx(Marshaler& m, bool wide);

    // -------------------------------------------------------------------------
    // Memory
    // -------------------------------------------------------------------------
    void impl_IsBadWritePtr(Marshaler& m);
    void impl_GlobalMemoryStatusEx(Marshaler& m);

    // -------------------------------------------------------------------------
    // Console
    // -------------------------------------------------------------------------
    void impl_GetConsoleCP(Marshaler& m);
    void impl_SetConsoleCP(Marshaler& m);
    void impl_ReadConsoleW(Marshaler& m);
    void impl_SetConsoleCtrlHandler(Marshaler& m);
    void impl_SetStdHandle(Marshaler& m);

    // -------------------------------------------------------------------------
    // Directory / Disk
    // -------------------------------------------------------------------------
    void impl_GetCurrentDirectoryA(Marshaler& m);
    void impl_RemoveDirectoryA(Marshaler& m);
    void impl_GetDriveTypeW(Marshaler& m);
    void impl_GetDiskFreeSpaceA(Marshaler& m);
    void impl_GetDiskFreeSpaceExW(Marshaler& m);
    /// GetDiskFreeSpaceA/W's shared body (contract measured on Windows in
    /// tools/oracle_probe/disk_free_probe.expected). @p path is null for a
    /// NULL lpRootPathName.
    void get_disk_free_space(Marshaler& m, const std::string* path);

    // -------------------------------------------------------------------------
    // Time / Date
    // -------------------------------------------------------------------------
    void impl_GetTimeZoneInformation(Marshaler& m);
    void impl_GetDynamicTimeZoneInformation(Marshaler& m);
    void impl_GetTimeZoneInformationForYear(Marshaler& m);
    void impl_LoadModule(Marshaler& m);
    void impl_GetDateFormatA(Marshaler& m);
    void impl_GetDateFormatW(Marshaler& m);
    /// Shared GetDateFormatA/W core. `wide` selects the output encoding.
    void get_date_format_common(Marshaler& m, bool wide);
    void impl_SystemTimeToTzSpecificLocalTime(Marshaler& m);
    void impl_TzSpecificLocalTimeToSystemTime(Marshaler& m);

    // -------------------------------------------------------------------------
    // Heap Extended
    // -------------------------------------------------------------------------
    void impl_HeapLock(Marshaler& m);
    void impl_HeapUnlock(Marshaler& m);
    void impl_HeapQueryInformation(Marshaler& m);
    void impl_HeapWalk(Marshaler& m);

    // -------------------------------------------------------------------------
    // Fiber
    // -------------------------------------------------------------------------

    // -------------------------------------------------------------------------
    // Version
    // -------------------------------------------------------------------------
    void impl_VerifyVersionInfoW(Marshaler& m);
    void impl_VerifyVersionInfoA(Marshaler& m);
    void impl_VerSetConditionMask(Marshaler& m);
    void verify_version_info(Marshaler& m, bool wide);

    // -------------------------------------------------------------------------
    // System Information (extended)
    // -------------------------------------------------------------------------
    void impl_GetLogicalProcessorInformationEx(Marshaler& m);

    // -------------------------------------------------------------------------
    // CRT startup probes (GetProcAddress at runtime)
    // -------------------------------------------------------------------------
    void impl_AreFileApisANSI(Marshaler& m);
    void impl_SetFileApisToANSI(Marshaler& m);
    void impl_SetFileApisToOEM(Marshaler& m);
    void impl_GetCurrentPackageId(Marshaler& m);
    void impl_GetCurrentPackageFullName(Marshaler& m);
    void impl_GetPackageFamilyName(Marshaler& m);
    void impl_GetPackagePathByFullName(Marshaler& m);
    void impl_GetPackagesByPackageFamily(Marshaler& m);
    void impl_CompareObjectHandles(Marshaler& m);
    void impl_PackageFamilyNameFromId(Marshaler& m);
    void impl_PackageNameAndPublisherIdFromFamilyName(Marshaler& m);
    void impl_GetSystemTimePreciseAsFileTime(Marshaler& m);
    void impl_SleepConditionVariableCS(Marshaler& m);
    void impl_GetTempPath2W(Marshaler& m);
    void impl_AppPolicyGetProcessTerminationMethod(Marshaler& m);
    void impl_AppPolicyGetThreadInitializationType(Marshaler& m);
    void impl_GetQueuedCompletionStatus(Marshaler& m);

    // -------------------------------------------------------------------------
    // CPU-Z: Priority / Process
    // -------------------------------------------------------------------------
    void impl_SetPriorityClass(Marshaler& m);
    void impl_GetPriorityClass(Marshaler& m);
    void impl_GetProcessTimes(Marshaler& m);
    void impl_GetSystemTimes(Marshaler& m);

    // -------------------------------------------------------------------------
    // CPU-Z: Profile / Locale / Resource
    // -------------------------------------------------------------------------
    void impl_GetProfileIntA(Marshaler& m);
    void impl_GetProfileIntW(Marshaler& m);
    void impl_ConvertDefaultLocale(Marshaler& m);
    void impl_FindResourceExW(Marshaler& m);
    void impl_EnumResourceLanguagesA(Marshaler& m);
    void impl_EnumResourceLanguagesW(Marshaler& m);

    // -------------------------------------------------------------------------
    // CPU-Z: File Locking
    // -------------------------------------------------------------------------
    void impl_LockFile(Marshaler& m);
    void impl_UnlockFile(Marshaler& m);

    // -------------------------------------------------------------------------
    // CPU-Z: Atom Table
    // -------------------------------------------------------------------------
    void impl_GlobalAddAtomW(Marshaler& m);
    void impl_GlobalFindAtomW(Marshaler& m);
    void impl_GlobalDeleteAtom(Marshaler& m);
    void impl_GlobalGetAtomNameW(Marshaler& m);

    // -------------------------------------------------------------------------
    // Communications (serial port) family: comm_port.cpp. ClearCommError,
    // EscapeCommFunction, GetCommModemStatus, SetCommState and SetCommTimeouts
    // are declared with the Wave 3 group below.
    // -------------------------------------------------------------------------
    void impl_PurgeComm(Marshaler& m);
    void impl_GetCommState(Marshaler& m);
    void impl_GetCommTimeouts(Marshaler& m);
    void impl_ClearCommBreak(Marshaler& m);
    void impl_SetCommBreak(Marshaler& m);
    void impl_GetCommMask(Marshaler& m);
    void impl_SetCommMask(Marshaler& m);
    void impl_GetCommProperties(Marshaler& m);
    void impl_GetCommConfig(Marshaler& m);
    void impl_SetCommConfig(Marshaler& m);
    void impl_SetupComm(Marshaler& m);
    void impl_TransmitCommChar(Marshaler& m);
    void impl_WaitCommEvent(Marshaler& m);
    void impl_GetDefaultCommConfigA(Marshaler& m);
    void impl_GetDefaultCommConfigW(Marshaler& m);
    void impl_SetDefaultCommConfigA(Marshaler& m);
    void impl_SetDefaultCommConfigW(Marshaler& m);
    void impl_GetCommPorts(Marshaler& m);
    void impl_OpenCommPort(Marshaler& m);

    // -------------------------------------------------------------------------
    // CPU-Z: Overlapped I/O
    // -------------------------------------------------------------------------
    void impl_GetOverlappedResult(Marshaler& m);
    void impl_GetOverlappedResultEx(Marshaler& m);
    /// GetOverlappedResult(Ex) core: @p timeout_ms 0 = no wait.
    void get_overlapped_result(Marshaler& m, u32 timeout_ms, bool alertable);
    /// Wait until OVERLAPPED.Internal leaves STATUS_PENDING (the file-object
    /// wait when hEvent is NULL). Returns WAIT_OBJECT_0 / WAIT_TIMEOUT /
    /// WAIT_IO_COMPLETION.
    [[nodiscard]] u32 wait_overlapped_internal(GuestAddr overlapped, bool is_64bit,
                                               u32 timeout_ms, bool alertable);
    /// WaitForSingleObjectEx core (alertable-aware), shared with
    /// GetOverlappedResultEx.
    [[nodiscard]] u32 wait_handle_ex(Handle handle, u32 milliseconds, bool alertable);
    void impl_CancelIo(Marshaler& m);
    void impl_CancelIoEx(Marshaler& m);
    void impl_CancelSynchronousIo(Marshaler& m);

    // -------------------------------------------------------------------------
    // CPU-Z: ANSI File/Directory APIs
    // -------------------------------------------------------------------------
    void impl_SetDllDirectoryA(Marshaler& m);
    void impl_SetDllDirectoryW(Marshaler& m);
    void impl_CreateDirectoryA(Marshaler& m);
    void impl_GetTempPathA(Marshaler& m);
    void impl_GetWindowsDirectoryA(Marshaler& m);
    void impl_GetSystemDirectoryA(Marshaler& m);

    // -------------------------------------------------------------------------
    // CPU-Z: Sync / Mutex / Semaphore
    // -------------------------------------------------------------------------
    void impl_ReleaseSemaphore(Marshaler& m);
    void impl_CreateSemaphoreW(Marshaler& m);
    void impl_QueueUserAPC(Marshaler& m);

    // -------------------------------------------------------------------------
    // CPU-Z: Toolhelp32 Snapshot
    // -------------------------------------------------------------------------
    void impl_CreateToolhelp32Snapshot(Marshaler& m);
    void impl_Process32First(Marshaler& m);
    void impl_Process32Next(Marshaler& m);
    void impl_Thread32First(Marshaler& m);
    void impl_Thread32Next(Marshaler& m);

    // -------------------------------------------------------------------------
    // CPU-Z: Interlocked / Memory
    // -------------------------------------------------------------------------
    void impl_InterlockedExchangeAdd(Marshaler& m);
    void impl_GlobalMemoryStatus(Marshaler& m);

private:
    void impl_CreateProcessCommon(Marshaler& m, usize argument_offset,
                                  std::optional<Handle> token,
                                  bool with_token_w_layout = false,
                                  bool ansi_strings = false);
    [[nodiscard]] int launch_guest_worker(
        const std::string& host_image_path,
        const std::string& guest_image_path,
        const std::string& guest_command_line,
        const std::string& guest_current_directory = {},
        const std::string& inherited_pipes = {},
        const std::string& inherited_sync = {},
        const std::string& inherited_mappings = {},
        std::string_view elevation_credential = {},
        std::vector<std::string> extra_env = {},
        std::vector<std::string> guest_environment = {},
        bool detached = false,
        /// "in,out" pseudoconsole pipe handles; the child's console becomes
        /// that pseudoconsole. Empty for an ordinary launch.
        const std::string& pseudo_console_handles = {});

    GuestMemory& m_memory;
    capsule::Vfs& m_vfs;
    ModuleManager* m_module_manager{nullptr};
    ConsoleManager* m_console_manager{nullptr};
    ProcessManager* m_process_manager{nullptr};
    HandleTable* m_handle_table{nullptr};

    struct ExternalPrimaryThread {
        ipc::ProcessObject* process{nullptr};
        u32 suspend_count{0};
        u32 thread_id{0};
    };
    std::mutex m_external_thread_mutex;
    /// CreateProcess hThread / DuplicateHandle / OpenThread handles → tid.
    std::unordered_map<Handle, u32> m_external_primary_handles;
    /// Durable by-tid record so OpenThread works after the CreateProcess
    /// handle is closed (process_contract_probe OpenThread-after-exit).
    std::unordered_map<u32, ExternalPrimaryThread> m_external_primary_by_tid;
    SecurityStubs* m_security_stubs{nullptr};
    ipc::ProcessSession* m_process_session{nullptr};
    std::function<void()> m_pre_process_launch;
    std::function<bool()> m_foreground_may_grant;
    std::function<void(int)> m_foreground_granted;
    const GuestElevationState* m_elevation{nullptr};
    platform::ElevationAuthorizer* m_elevation_authorizer{nullptr};
    SyncManager* m_sync_manager{nullptr};
    CriticalSectionManager* m_critical_section_manager{nullptr};
    ThreadManager* m_thread_manager{nullptr};
    /// SetThreadStackGuarantee bytes, keyed by the calling thread's TEB
    /// address. Windows keeps this in TEB->GuaranteedStackBytes; Vitreo keys
    /// off the TEB rather than writing that undocumented offset. Guarded by
    /// m_thread_stack_guarantee_mutex because guest threads run concurrently.
    std::unordered_map<GuestAddr, u32> m_thread_stack_guarantee;
    mutable std::mutex m_thread_stack_guarantee_mutex;
    /// Mask set by SetProcessAffinityMask; 0 means unrestricted, in which case
    /// GetProcessAffinityMask reports the full system mask.
    u64 m_process_affinity_mask{0};
    /// SetPriorityClass storage for Process handles that are not the current
    /// process (self priority lives on ProcessManager).
    std::unordered_map<ipc::ProcessObject*, u32> m_remote_priority_class;
    /// Set/GetPriorityClass on a remote process handle run on any guest
    /// thread; the table is touched only under this lock.
    std::mutex m_priority_class_mutex;
    EnvironmentManager* m_environment_manager{nullptr};
    FileSearchManager* m_file_search_manager{nullptr};
    NtdllExtManager* m_ntdll_ext_manager{nullptr};
    SysInfoManager* m_sysinfo_manager{nullptr};
    FileManager* m_file_manager{nullptr};
    /// ReadDirectoryChangesW tracking (directory_changes.cpp), created on use.
    std::shared_ptr<DirectoryChangeService> m_directory_changes;
    std::once_flag m_directory_changes_once;
    DirectoryChangeService& directory_changes();
    FileMappingManager* m_file_mapping_manager{nullptr};
    VirtualMemoryManager* m_virtual_memory_manager{nullptr};
    GlobalLocalManager* m_global_local_manager{nullptr};
    GuestCall m_guest_call;
    SocketHandleProbe m_socket_handle_probe;
    SocketIoCancel m_socket_io_cancel;
    DeviceIoCancel m_device_io_cancel;
    std::mutex m_init_once_mutex;
    std::condition_variable m_init_once_cv;
    u32 m_error_mode{0};
    /// GetTempFileName: generating calls that got past the directory check;
    /// the first number tried is (GetTickCount() + this) & 0xFFFF.
    std::atomic<u32> m_temp_counter{0};
    struct ActivationContext {
        u32 refs{1};
    };
    std::mutex m_actctx_mutex;
    std::unordered_map<u32, ActivationContext> m_actctxs;
    u32 m_next_actctx{1};
    u32 m_pointer_cookie{0};  ///< CV-12: Random XOR cookie for EncodePointer/DecodePointer
    u32 m_event_counter{0};
    struct RegisteredWaitState {
        std::atomic_bool cancelled{false};
        std::mutex mutex;
        std::condition_variable cv;
        bool finalized{false};
        /// A WT_EXECUTEINWAITTHREAD callback the wait thread is waiting on.
        bool inline_running{false};
        Handle completion_event{NULL_HANDLE};
        std::vector<Handle> callback_threads;
    };
    /// Stop a registered wait: TRUE, or FALSE + ERROR_IO_PENDING while a
    /// callback still runs (s4_threadpool_probe rw_unregister_*).
    [[nodiscard]] bool unregister_wait_state(
        const std::shared_ptr<RegisteredWaitState>& state, Handle completion);
    std::mutex m_registered_wait_mutex;
    std::unordered_map<u32, std::shared_ptr<RegisteredWaitState>>
        m_registered_waits;

    // Timer-queue timers (CreateTimerQueueTimer). Each timer owns a tracked
    // host thread that schedules its guest WAITORTIMERCALLBACK and, once the
    // timer is deleted, waits for its callbacks and signals the completion
    // event. Contract measured by s4_threadpool_probe (ctqt_* / dtqt_*).
    struct TimerQueueTimerState {
        std::mutex mutex;
        std::condition_variable cv;
        u32 queue{0};                 ///< 0 = default timer queue
        bool deleted{false};
        bool finished{false};         ///< callbacks drained after delete
        bool expired_oneshot{false};  ///< Change is then a no-op (Learn)
        bool once{false};             ///< WT_EXECUTEONLYONCE
        bool in_timer_thread{false};  ///< WT_EXECUTEINTIMERTHREAD
        u64 generation{0};            ///< bumped by ChangeTimerQueueTimer
        std::chrono::steady_clock::time_point due{};
        u32 period_ms{0};
        Handle completion_event{NULL_HANDLE};
        std::vector<Handle> callback_threads;  ///< running or unreaped
    };
    /// True when any recorded callback thread is still running; closes the
    /// handles of the ones that have finished. Caller holds state.mutex.
    [[nodiscard]] bool reap_callback_threads(std::vector<Handle>& threads);
    /// Mark @p state deleted; returns the Win32 result of the delete given
    /// the caller's CompletionEvent.
    [[nodiscard]] bool delete_timer_state(
        const std::shared_ptr<TimerQueueTimerState>& state, Handle completion,
        bool& io_pending);
    std::atomic<u32> m_timer_queue_counter{0};
    std::mutex m_timer_queue_mutex;
    std::unordered_map<u32, std::shared_ptr<TimerQueueTimerState>>
        m_timer_queue_timers;

    u32 m_iocp_counter{0};
    std::string m_current_directory{"C:\\"};
    /// SetSearchPathMode state: 0 = never set (the registry default,
    /// SafeProcessSearchMode = 0, applies), 1 = safe, 2 = not safe; and
    /// whether safe mode was made permanent.
    std::atomic<u32> m_search_path_mode{0};
    std::atomic<bool> m_search_path_permanent{false};
    /// Open handle on the current directory (Learn SetCurrentDirectory note:
    /// the directory is locked while current; share R|W, no delete).
    u32 m_cwd_handle = 0;  // capsule::FileHandle; 0 = INVALID_FILE_HANDLE
    std::string m_capsule_name{"default"};

    // Atom table for GlobalAddAtomW / GlobalFindAtomW / GlobalGetAtomNameW.
    // The global atom table is a kernel object shared by every thread; the
    // counter and the map are touched only under m_atom_mutex, held for the
    // table operation (the guest-memory copy in GetAtomName included; it is
    // not a wait).
    u16 m_next_atom{0xC001};
    std::unordered_map<u16, std::wstring> m_atoms;
    std::mutex m_atom_mutex;

    // Semaphore handle counter
    u32 m_semaphore_counter{0};

    // Toolhelp snapshot handle counter
    u32 m_toolhelp_handle{0xFACE0000};
    struct ToolhelpModuleEntry {
        GuestAddr base_address{0};
        u32 image_size{0};
        std::string name;
        std::string path;
    };
    struct ToolhelpSnapshot {
        std::vector<ipc::ProcessSnapshotEntry> processes;
        std::vector<ToolhelpModuleEntry> modules;
        std::vector<u32> threads; // th32ThreadID values (current process)
        usize next_index{0};
        usize next_module_index{0};
        usize next_thread_index{0};
    };
    ProcessSnapshotProvider m_process_snapshot_provider;
    std::unordered_map<u32, ToolhelpSnapshot> m_toolhelp_snapshots;
    /// CreateToolhelp32Snapshot inserts while another thread's
    /// Module32Next / Process32Next / Thread32Next walks its own snapshot
    /// (overlay hooks and crash reporters enumerate from worker threads).
    /// Held for the whole *32First/Next call: the cursor lives in the entry.
    std::mutex m_toolhelp_mutex;

    // I/O Completion Port infrastructure
    struct CompletionEntry {
        u32 bytes_transferred;
        u64 completion_key;   // ULONG_PTR — full width (8 bytes in x64)
        GuestAddr overlapped; // LPOVERLAPPED — full width (8 bytes in x64)
        u32 nt_status;
        u32 win_error;
    };
    struct CompletionPort {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<CompletionEntry> queue;
        bool closed{false};
    };
    struct FileCompletionAssociation {
        u32 port_handle;
        u64 completion_key;
    };
    std::mutex m_iocp_registry_mutex;
    std::unordered_map<u32, std::shared_ptr<CompletionPort>> m_iocp_ports;
    std::unordered_map<Handle, FileCompletionAssociation> m_iocp_file_associations;
    std::unordered_map<Handle, u8> m_file_completion_modes;
    struct PendingFileCompletion {
        std::shared_ptr<CompletionPort> port;
        u32 port_handle;
        u64 completion_key;
        u8 modes;
    };
    std::unordered_map<GuestAddr, PendingFileCompletion> m_pending_file_completions;


    /// HANDLE_FLAG_* for Winsock socket handles, which live outside HandleTable.
    std::mutex m_socket_handle_flags_mutex;
    std::unordered_map<Handle, u32> m_socket_handle_flags;
    struct TrackedOverlappedIo {
        u32 process_id{0};
        u32 submitting_thread_id{0};
        Handle submitted_handle{NULL_HANDLE};
        u64 object_id{0};
        GuestAddr overlapped{0};
        GuestAddr buffer{0};
        u32 requested_bytes{0};
        u32 transferred_bytes{0};
        u32 completion_port{0};
        u64 completion_key{0};
        u64 event_handle{0};
        u32 win32_error{0};
        u32 nt_status{0};
        u8 completion_modes{0};
        OverlappedIoTraceStage stage;
        bool association_found{false};
        bool contains_ping{false};
    };
    std::mutex m_overlapped_trace_mutex;
    std::unordered_map<GuestAddr, TrackedOverlappedIo> m_overlapped_traces;
    std::atomic<bool> m_shutdown{false};

    // SetFileApisToOEM/SetFileApisToANSI select the code page the ANSI file
    // APIs translate through; it is per-process state, and AreFileApisANSI is
    // the only way a caller can read it back. ANSI is the process default.
    std::atomic<bool> m_file_apis_ansi{true};

    // FindFirstChangeNotification* watchers. The waitable object is an
    // auto-reset SyncManager event so WaitFor*/RegisterWait work per MSDN.
    struct ChangeNotificationWatch {
        Handle event_handle{NULL_HANDLE};
        std::string host_path;
        bool watch_subtree{false};
        u32 notify_filter{0};
        std::atomic<bool> stop{false};
        std::atomic<bool> armed{true};
        std::atomic<bool> pending{false};
        std::thread worker;
    };
    std::mutex m_change_watch_mutex;
    std::unordered_map<Handle, std::shared_ptr<ChangeNotificationWatch>>
        m_change_watches;
    void stop_change_notification_watch(
        const std::shared_ptr<ChangeNotificationWatch>& watch);
    void stop_all_change_notification_watches();
    [[nodiscard]] Handle start_change_notification(const std::string& path,
                                                   bool watch_subtree,
                                                   u32 notify_filter);

    /// CreateMemoryResourceNotification handle → type (0=Low, 1=High).
    std::mutex m_mrn_mutex;
    std::unordered_map<Handle, u32> m_mrn_types;

    void record_overlapped_trace(const OverlappedIoTraceEvent& event);
    void record_completion_trace(OverlappedIoTraceStage stage,
                                 Handle file_handle,
                                 GuestAddr overlapped,
                                 u32 bytes_transferred,
                                 u32 port_handle,
                                 u64 completion_key,
                                 u64 event_handle,
                                 u8 completion_modes,
                                 bool association_found,
                                 u32 nt_status,
                                 u32 win32_error);
    void dump_stuck_overlapped_traces();
    [[nodiscard]] bool enqueue_completion_packet(
        u32 port_handle, CompletionEntry entry);

    struct ProcThreadAttributeListState {
        u32 capacity{0};
        u32 attribute_count{0};
        std::unordered_set<u64> attributes;
        std::optional<std::vector<Handle>> inherited_handles;
        std::optional<std::vector<Handle>> job_handles;
        std::array<u64, 2> mitigation_policy{};
        u32 mitigation_policy_size{0};
        std::optional<u32> child_process_policy;
        /// PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE. The child's console I/O is
        /// the pseudoconsole's pipes rather than this process's console.
        std::optional<u64> pseudo_console;
    };
    std::mutex m_proc_thread_attribute_mutex;
    std::unordered_map<GuestAddr, ProcThreadAttributeListState>
        m_proc_thread_attribute_lists;
    bool m_child_process_restricted{false};
    std::array<u64, 2> m_process_creation_mitigation_policy{};

    // Resource directory info (from PE headers)
    GuestAddr m_rsrc_image_base{0};
    u32 m_rsrc_rva{0};
    u32 m_rsrc_size{0};
    GuestAddr m_enum_resource_scratch{0};

    // HRSRC handle map: handle → (guest_addr, size)
    struct ResourceInfo {
        GuestAddr guest_addr;
        u32 size;
    };
    u32 m_next_hrsrc{0xB0000001};
    std::unordered_map<u32, ResourceInfo> m_resource_handles;
    /// FindResource inserts from any thread while LoadResource / LockResource
    /// / SizeofResource look up on others; counter and map share the lock.
    std::mutex m_resource_mutex;

    /// Write a UTF-16LE literal (ASCII chars) to guest memory with null terminator
    /// @return Number of characters written (excluding null terminator)
    u32 write_wide_literal(GuestAddr addr, const char* str);

    /// Find the length of a wide string in guest memory (in chars, excluding null)
    u32 guest_wstrlen(GuestAddr addr);

    /// Read a UTF-16LE string from guest memory, best-effort ASCII
    std::string read_wide_ascii_z(GuestAddr addr, usize max_chars = 4096);

    void impl_SearchPathCommon(Marshaler& m, bool wide);

public:
    // -------------------------------------------------------------------------
    // Additional Win32 stubs
    // -------------------------------------------------------------------------
    // Condition variable / synchronization
    void impl_InitializeConditionVariable(Marshaler& m);
    void impl_WakeConditionVariable(Marshaler& m);

    // Thread / process / module
    void impl_FreeLibraryAndExitThread(Marshaler& m);
    void impl_GetProcessId(Marshaler& m);
    void impl_CreateProcessA(Marshaler& m);

    // Sync (ANSI)
    void impl_CreateSemaphoreA(Marshaler& m);

    // Disk / volume (ANSI + W)
    void impl_GetDriveTypeA(Marshaler& m);
    void impl_GetDiskFreeSpaceExA(Marshaler& m);
    void impl_GetVolumeInformationA(Marshaler& m);
    void impl_GetLogicalDriveStringsA(Marshaler& m);
    void impl_GetVolumePathNameW(Marshaler& m);
    void impl_GetVolumeNameForVolumeMountPointW(Marshaler& m);

    // File ops (ANSI)

    // INI / profile
    void impl_GetPrivateProfileStringA(Marshaler& m);
    /// GetProfileStringA/W: GetPrivateProfileString on C:\Windows\win.ini.
    void impl_GetProfileStringA(Marshaler& m);
    void impl_GetProfileStringW(Marshaler& m);
    void private_profile_string_a(Marshaler& m, GuestAddr file_addr,
                                  const std::string& file_override);
    void private_profile_string_w(Marshaler& m, GuestAddr file_addr,
                                  const std::string& file_override);
    void impl_GetPrivateProfileSectionA(Marshaler& m);
    void impl_GetPrivateProfileSectionNamesA(Marshaler& m);
    void impl_WritePrivateProfileStringA(Marshaler& m);
    void impl_WritePrivateProfileSectionA(Marshaler& m);

    // Misc / system info
    void impl_K32GetModuleFileNameExA(Marshaler& m);
    void impl_WTSGetActiveConsoleSessionId(Marshaler& m);
    void impl_GetComputerNameExW(Marshaler& m);
    void impl_SetThreadExecutionState(Marshaler& m);
    void impl_SetProcessShutdownParameters(Marshaler& m);
    void impl_SetThreadErrorMode(Marshaler& m);
    void impl_GetSystemPowerStatus(Marshaler& m);

    // Default-fail stubs
    void impl_AttachConsole(Marshaler& m);
    void impl_GenerateConsoleCtrlEvent(Marshaler& m);
    void impl_DebugActiveProcess(Marshaler& m);
    void impl_DebugActiveProcessStop(Marshaler& m);
    void impl_Module32First(Marshaler& m);
    void impl_Module32Next(Marshaler& m);
    void impl_GetBinaryTypeW(Marshaler& m);
    void impl_GetBinaryTypeA(Marshaler& m);
    void get_binary_type_impl(Marshaler& m, bool wide);
    void impl_CreateHardLinkA(Marshaler& m);
    void impl_CreateHardLinkW(Marshaler& m);
    void create_hard_link_impl(Marshaler& m, bool wide);
    void impl_ReadDirectoryChangesW(Marshaler& m);
    void impl_FindFirstChangeNotificationA(Marshaler& m);
    void impl_FindFirstChangeNotificationW(Marshaler& m);
    void impl_FindNextChangeNotification(Marshaler& m);
    void impl_FindCloseChangeNotification(Marshaler& m);
    void impl_EnumResourceNamesW(Marshaler& m);
    void impl_EnumResourceNamesA(Marshaler& m);
    void enum_resource_names_impl(Marshaler& m, bool wide);
    void enum_resource_languages_impl(Marshaler& m, bool wide);

    // Vectored exception handlers (errhandlingapi.h)
    void impl_AddVectoredExceptionHandler(Marshaler& m);
    void impl_RemoveVectoredExceptionHandler(Marshaler& m);

    // Additional sync / thread / proc-thread-attribute-list APIs (wave 1)
    void impl_InterlockedPopEntrySList(Marshaler& m);
    void impl_InitOnceExecuteOnce(Marshaler& m);
    void impl_RtlRunOnceExecuteOnce(Marshaler& m);
    void execute_once(Marshaler& m, bool native_status);
    void impl_RegisterWaitForSingleObject(Marshaler& m);
    void impl_UnregisterWait(Marshaler& m);
    void impl_UnregisterWaitEx(Marshaler& m);
    void impl_CreateTimerQueue(Marshaler& m);
    void impl_CreateTimerQueueTimer(Marshaler& m);
    void impl_ChangeTimerQueueTimer(Marshaler& m);
    void impl_DeleteTimerQueueTimer(Marshaler& m);
    void impl_DeleteTimerQueueEx(Marshaler& m);
    void impl_DeleteTimerQueue(Marshaler& m);
    void impl_GetProcessIdOfThread(Marshaler& m);
    void impl_GetThreadId(Marshaler& m);
    void impl_SetThreadDescription(Marshaler& m);
    void impl_GetThreadDescription(Marshaler& m);
    [[nodiscard]] std::optional<u32> external_thread_description_status(
        Handle handle, u32 required_access);
    void impl_GetThreadTimes(Marshaler& m);
    void impl_GetThreadContext(Marshaler& m);
    void impl_SetThreadContext(Marshaler& m);
    void impl_GetThreadGroupAffinity(Marshaler& m);
    void impl_SetThreadGroupAffinity(Marshaler& m);
    void impl_GetThreadIdealProcessorEx(Marshaler& m);
    void impl_SetThreadIdealProcessorEx(Marshaler& m);
    void impl_GetCurrentProcessorNumberEx(Marshaler& m);
    void impl_SetThreadInformation(Marshaler& m);
    void impl_QueryThreadCycleTime(Marshaler& m);
    void impl_QueryProcessCycleTime(Marshaler& m);
    void impl_InitializeProcThreadAttributeList(Marshaler& m);
    void impl_UpdateProcThreadAttribute(Marshaler& m);
    void impl_DeleteProcThreadAttributeList(Marshaler& m);

    // Wave 3: Process/Job
    void impl_CreateJobObjectA(Marshaler& m);
    void impl_CreateJobObjectW(Marshaler& m);
    void impl_AssignProcessToJobObject(Marshaler& m);
    void impl_IsProcessInJob(Marshaler& m);
    void impl_QueryInformationJobObject(Marshaler& m);
    void impl_SetInformationJobObject(Marshaler& m);
    void impl_TerminateJobObject(Marshaler& m);
    bool on_job_handle_close(Handle handle);
    void impl_ReadProcessMemory(Marshaler& m);
    void impl_WriteProcessMemory(Marshaler& m);
    void impl_VirtualAllocEx(Marshaler& m);
    void impl_VirtualFreeEx(Marshaler& m);
    void impl_VirtualProtectEx(Marshaler& m);
    void impl_VirtualQueryEx(Marshaler& m);
    void impl_CreateRemoteThread(Marshaler& m);
    void impl_CreateRemoteThreadEx(Marshaler& m);
    void impl_GetProcessHandleCount(Marshaler& m);
    void impl_GetProcessWorkingSetSizeEx(Marshaler& m);
    void impl_GetProcessWorkingSetSize(Marshaler& m);
    void impl_SetProcessWorkingSetSize(Marshaler& m);
    void impl_SetProcessWorkingSetSizeEx(Marshaler& m);
    void impl_GetProcessMitigationPolicy(Marshaler& m);
    void impl_SetProcessMitigationPolicy(Marshaler& m);
    void impl_SetProcessDEPPolicy(Marshaler& m);
    void impl_SetProcessInformation(Marshaler& m);
    void impl_GetProcessInformation(Marshaler& m);
    void impl_CheckRemoteDebuggerPresent(Marshaler& m);
    // Wave 3: Console
    void impl_AllocConsole(Marshaler& m);
    void impl_FreeConsole(Marshaler& m);
    void impl_GetConsoleDisplayMode(Marshaler& m);
    void impl_GetConsoleWindow(Marshaler& m);
    void impl_CreateSemaphoreExW(Marshaler& m);
    void impl_CreateSemaphoreExA(Marshaler& m);
    // Wave 3: File/Volume/Path
    void impl_FindFirstFileExA(Marshaler& m);
    void impl_FindFirstVolumeW(Marshaler& m);
    void impl_FindNextVolumeW(Marshaler& m);
    void impl_FindVolumeClose(Marshaler& m);
    void impl_GetFinalPathNameByHandleW(Marshaler& m);
    void impl_GetFinalPathNameByHandleA(Marshaler& m);
    void impl_GetFullPathNameA(Marshaler& m);
    void impl_GetTempFileNameA(Marshaler& m);
    void impl_QueryDosDeviceW(Marshaler& m);
    void impl_QueryDosDeviceA(Marshaler& m);
    void query_dos_device_export(Marshaler& m, bool wide);
    void impl_LockFileEx(Marshaler& m);
    void impl_UnlockFileEx(Marshaler& m);
    // Wave 3: System Info
    void impl_GetFirmwareType(Marshaler& m);
    void impl_GetMaximumProcessorCount(Marshaler& m);
    void impl_GetMaximumProcessorGroupCount(Marshaler& m);
    void impl_GetActiveProcessorCount(Marshaler& m);
    void impl_GetActiveProcessorGroupCount(Marshaler& m);
    void impl_GetPhysicallyInstalledSystemMemory(Marshaler& m);
    void impl_GetLogicalProcessorInformation(Marshaler& m);
    void impl_CreateMemoryResourceNotification(Marshaler& m);
    void impl_QueryMemoryResourceNotification(Marshaler& m);
    void impl_DiscardVirtualMemory(Marshaler& m);
    void impl_PrefetchVirtualMemory(Marshaler& m);
    void impl_SetDefaultDllDirectories(Marshaler& m);
    void impl_AddDllDirectory(Marshaler& m);
    void impl_RemoveDllDirectory(Marshaler& m);
    // Wave 3: Named Pipes & Comm
    void impl_CreateNamedPipeA(Marshaler& m);
    void impl_CreateNamedPipeW(Marshaler& m);
    /// Shared body of CreateNamedPipeA/W: @p wide selects how lpName is read.
    void create_named_pipe_impl(Marshaler& m, bool wide);
    void impl_WaitNamedPipeA(Marshaler& m);
    void impl_WaitNamedPipeW(Marshaler& m);
    void impl_CallNamedPipeA(Marshaler& m);
    void impl_CallNamedPipeW(Marshaler& m);
    void impl_SetNamedPipeHandleState(Marshaler& m);
    void impl_GetNamedPipeHandleStateW(Marshaler& m);
    void impl_GetNamedPipeInfo(Marshaler& m);
    void impl_ConnectNamedPipe(Marshaler& m);
    void impl_DisconnectNamedPipe(Marshaler& m);
    void impl_TransactNamedPipe(Marshaler& m);
    void impl_GetNamedPipeClientProcessId(Marshaler& m);
    void impl_GetNamedPipeServerProcessId(Marshaler& m);
    void impl_ClearCommError(Marshaler& m);
    void impl_EscapeCommFunction(Marshaler& m);
    void impl_GetCommModemStatus(Marshaler& m);
    void impl_SetCommState(Marshaler& m);
    void impl_SetCommTimeouts(Marshaler& m);
    // Wave 3: Fiber
    // Wave 3: PSAPI K32*
    void impl_K32EnumProcesses(Marshaler& m);
    void impl_K32GetModuleBaseNameA(Marshaler& m);
    void impl_K32GetModuleBaseNameW(Marshaler& m);
    void impl_K32EnumProcessModules(Marshaler& m);
    void impl_K32GetModuleFileNameExW(Marshaler& m);
    void impl_K32GetModuleInformation(Marshaler& m);
    void impl_K32GetPerformanceInfo(Marshaler& m);
    void impl_K32GetProcessMemoryInfo(Marshaler& m);
    /// K32GetProcessImageFileNameA/W (and psapi's GetProcessImageFileName):
    /// the process image as an NT device path. Measured
    /// (process_image_name_probe, x86 = x64): the length without the
    /// terminator, last error untouched; a buffer without room for the path
    /// and its terminator is 0 / ERROR_INSUFFICIENT_BUFFER, untouched; a
    /// NULL, unknown or non-process handle is 0 / ERROR_INVALID_HANDLE.
    void impl_K32GetProcessImageFileName(Marshaler& m, bool wide);
    // Shared by the K32* names and psapi.dll's forwarders (psapi_k32.cpp).
    [[nodiscard]] std::optional<std::string> psapi_module_path(
        Marshaler& m, u64 process, GuestAddr module, bool needs_vm_read,
        const char* api);
    void psapi_module_base_name(Marshaler& m, bool wide);
    void psapi_module_file_name(Marshaler& m, bool wide);
    void impl_K32QueryWorkingSetEx(Marshaler& m);
    void impl_Module32FirstW(Marshaler& m);
    void impl_Module32NextW(Marshaler& m);
    void impl_Process32FirstW(Marshaler& m);
    void impl_Process32NextW(Marshaler& m);

private:

    /// A process in a job. Membership belongs to the process, not to the
    /// handle value used to assign it (another handle to the same process,
    /// or the pseudo handle, must see the same answer).
    struct JobMember {
        pid_t pid{0};
        ipc::ProcessObject* process{nullptr};  ///< nullptr: this process
        bool exited{false};
        bool killed{false};  ///< ended by TerminateJobObject / job close
    };
    struct JobState {
        std::vector<u8> extended_limits;
        u32 ui_restrictions{0};
        std::vector<JobMember> members;
        u32 completion_port{0};
        u64 completion_key{0};
        std::unordered_set<pid_t> notified_processes;
        /// Stops this job's member-exit watchers (job destroyed / teardown).
        std::shared_ptr<std::atomic<bool>> watch_cancel =
            std::make_shared<std::atomic<bool>>(false);
    };
    /// Add @p process_handle's process to @p job: NEW_PROCESS message and an
    /// exit watcher. False for a handle that is not a process.
    bool add_job_member(const std::shared_ptr<JobState>& job, Handle process_handle);
    void notify_job_new_process(const std::shared_ptr<JobState>& job, pid_t pid);
    void on_job_member_exit(const std::shared_ptr<JobState>& job, pid_t pid,
                            u32 exit_code);
    /// Terminate every live member; @p include_self also ends this process.
    void terminate_job_members(const std::shared_ptr<JobState>& job,
                               u32 exit_code, bool include_self);
    [[nodiscard]] bool job_kills_on_close(const JobState& job) const;
    [[nodiscard]] pid_t pid_for_process_handle(Handle process_handle) const;
    /// Notified after every overlapped completion is published
    /// (GetOverlappedResult with no hEvent waits on it).
    std::mutex m_overlapped_done_mutex;
    std::condition_variable m_overlapped_done_cv;
    std::mutex m_job_mutex;
    std::unordered_map<Handle, std::shared_ptr<JobState>> m_jobs;
    /// Host threads waiting for job members to exit (joined when done, when
    /// the next one starts, and in the destructor).
    struct JobWatcher {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex m_job_watchers_mutex;
    std::vector<JobWatcher> m_job_watchers;
    u32 m_pipe_counter{0};
    // Declared last so worker jthreads stop and join before any state they use
    // is destroyed.
    std::mutex m_pipe_connect_workers_mutex;
    struct PipeConnectWorker {
        std::shared_ptr<std::atomic<bool>> done;
        std::jthread thread;  // declared last: joins before `done` goes
    };
    std::vector<PipeConnectWorker> m_pipe_connect_workers;
};

// =============================================================================
// Export Table Factory
// =============================================================================

/// Build DllExports for extended kernel32 thunks
/// @param mgr Reference to Kernel32ExtManager (captured by lambdas)
/// @return DllExports containing all kernel32-ext entries
[[nodiscard]] DllExports create_kernel32_ext_thunks(Kernel32ExtManager& mgr);

} // namespace vitreo::winapi
