#pragma once

/// @file process.hpp
/// @brief ExitProcess and Process/Thread Info emulation (WIN32-031)
///
/// Provides synthetic process and thread identity, debugger detection,
/// startup info, and process exit handling for guest Win32 binaries.

#include <vitreo/common.hpp>
#include "ipc/process_registry.hpp"
#include "core/memory/guest_memory.hpp"
#include <string_view>
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <utility>

namespace vitreo::winapi {

// Forward declarations
class Marshaler;
class HandleTable;
class ModuleManager;
class GlobalLocalManager;
struct DllExports;

// =============================================================================
// Process Pseudo-Handle Constants
// =============================================================================

/// Pseudo-handle returned by GetCurrentProcess() (matches Windows)
constexpr u32 CURRENT_PROCESS_PSEUDO = static_cast<u32>(-1);

/// Pseudo-handle returned by GetCurrentThread() (matches Windows)
constexpr u32 CURRENT_THREAD_PSEUDO  = static_cast<u32>(-2);

// =============================================================================
// STARTUPINFOW / STARTUPINFOA Layout
// =============================================================================
//
// The structure ends in four pointer-width fields (lpReserved2 and the three
// std handles) preceded by pointer-width string pointers, so its size differs
// per bitness. Writing the 32-bit size into a 64-bit guest's buffer leaves
// lpReserved2 (offset 72) and hStdInput/hStdOutput/hStdError (80/88/96) holding
// whatever the caller's stack contained. The MSVC CRT's lowio initializer reads
// cbReserved2/lpReserved2 to adopt an inherited file-handle block, so that
// uninitialized tail makes it abandon lowio setup and leave __pioinfo empty.

/// Size of a 32-bit STARTUPINFOW/STARTUPINFOA structure in bytes
constexpr usize STARTUPINFOW_SIZE_32 = 68;

/// Size of a 64-bit STARTUPINFOW/STARTUPINFOA structure in bytes
constexpr usize STARTUPINFOW_SIZE_64 = 104;

/// The STARTUPINFO the creating process passed to CreateProcess, as this
/// process's GetStartupInfoA/W must report it (Learn STARTUPINFOW;
/// process_contract_probe.expected: the child sees dwFlags 0x101,
/// wShowWindow 7 and hStd* equal to GetStdHandle). All zero for a process
/// launched from the host. GetStartupInfo reports lpTitle/lpDesktop as NULL;
/// Windows reports the creator's lpTitle, or the image path when it passed
/// none (window_propstore_probe64.expected "child ... lpTitle"). The creator's
/// lpTitle reaches the child only as VITREO_STARTUP_TITLE, where
/// STARTF_TITLEISAPPID makes it the explicit AppUserModelID.
struct GuestStartupInfo {
    u32 x{0}, y{0}, x_size{0}, y_size{0};
    u32 x_count_chars{0}, y_count_chars{0}, fill_attribute{0};
    u32 flags{0};
    u16 show_window{0};
    u32 std_input{0}, std_output{0}, std_error{0};
};
constexpr u32 STARTF_USESTDHANDLES = 0x00000100;

/// Process-wide startup info (set once at startup by the runtime).
[[nodiscard]] GuestStartupInfo& guest_startup_info() noexcept;

/// Serialize / parse the startup info carried to a child worker in the
/// VITREO_STARTUP_INFO host environment variable.
[[nodiscard]] std::string encode_guest_startup_info(const GuestStartupInfo& si);
[[nodiscard]] bool decode_guest_startup_info(std::string_view text,
                                             GuestStartupInfo& si);

/// Write a STARTUPINFOA/W at the guest's pointer width from
/// guest_startup_info(); the A and W layouts are identical.
void write_guest_startup_info(GuestMemory& memory, GuestAddr addr, bool is_64bit);

// =============================================================================
// ProcessManager Class
// =============================================================================

/// Manages process and thread identity for a single guest process
///
/// Provides implementations for:
/// - GetCurrentProcess / GetCurrentProcessId
/// - GetCurrentThread / GetCurrentThreadId
/// - IsDebuggerPresent
/// - ExitProcess
/// - GetStartupInfoW
///
/// The guest process ID is the backing Vitreo host process ID so independently
/// running guests can discover each other. Thread IDs remain guest-local.
class ProcessManager {
public:
    using ProcessSnapshotProvider =
        std::function<std::vector<ipc::ProcessSnapshotEntry>()>;

    /// Construct with the default guest thread identity
    ProcessManager() = default;

    /// No copy
    ProcessManager(const ProcessManager&) = delete;
    ProcessManager& operator=(const ProcessManager&) = delete;

    /// No move (may be referenced by thunks)
    ProcessManager(ProcessManager&&) = delete;
    ProcessManager& operator=(ProcessManager&&) = delete;

    /// Destructor
    ~ProcessManager() = default;

    // -------------------------------------------------------------------------
    // Process Identity
    // -------------------------------------------------------------------------

    /// GetCurrentProcess — returns pseudo-handle (not a real handle)
    [[nodiscard]] u32 get_current_process() const noexcept;

    /// GetCurrentProcessId — returns the backing Vitreo host PID
    [[nodiscard]] u32 get_current_process_id() const noexcept;

    void set_process_snapshot_provider(ProcessSnapshotProvider provider) {
        m_process_snapshot_provider = std::move(provider);
    }

    /// Supply the loader's module table so a guest process that ends itself
    /// can report which module made that decision.
    void set_module_manager(ModuleManager* modules) noexcept {
        m_module_manager = modules;
    }

    [[nodiscard]] const ModuleManager* module_manager() const noexcept {
        return m_module_manager;
    }
    [[nodiscard]] ModuleManager* module_manager() noexcept {
        return m_module_manager;
    }

    [[nodiscard]] std::optional<ipc::ProcessSnapshotEntry> find_process(
        u32 pid) const;

    // -------------------------------------------------------------------------
    // Thread Identity
    // -------------------------------------------------------------------------

    /// GetCurrentThread — returns pseudo-handle (not a real handle)
    [[nodiscard]] u32 get_current_thread() const noexcept;

    /// GetCurrentThreadId — returns synthetic TID
    [[nodiscard]] u32 get_current_thread_id() const noexcept;

    // -------------------------------------------------------------------------
    // Debugger Detection
    // -------------------------------------------------------------------------

    /// IsDebuggerPresent — always returns false (no debugger attached)
    [[nodiscard]] bool is_debugger_present() const noexcept;

    // -------------------------------------------------------------------------
    // Process Lifetime
    // -------------------------------------------------------------------------

    /// ExitProcess — record exit code and set exited flag
    /// @param exit_code Win32 exit code (0 = success)
    void exit_process(u32 exit_code) noexcept;

    /// Optional wake invoked once from exit_process (e.g. CriticalSection
    /// enter_slow escape). Idempotent callee; may be null.
    void set_exit_wake(std::function<void()> wake) { m_exit_wake = std::move(wake); }

    /// Check whether ExitProcess has been called
    [[nodiscard]] bool has_exited() const noexcept;

    /// Get the exit code set by ExitProcess (0 if not yet exited)
    [[nodiscard]] u32 exit_code() const noexcept;

    /// Clear pending process-exit state (compatibility paths)
    void clear_exit_request() noexcept;

    // -------------------------------------------------------------------------
    // Priority class (SetPriorityClass / GetPriorityClass)
    // -------------------------------------------------------------------------

    /// Stored Win32 priority class (NORMAL_PRIORITY_CLASS by default).
    [[nodiscard]] u32 priority_class() const noexcept { return m_priority_class; }
    void set_priority_class(u32 cls) noexcept { m_priority_class = cls; }
    /// SetProcessInformation / GetProcessInformation(ProcessMemoryPriority).
    [[nodiscard]] u32 memory_priority() const noexcept {
        return m_memory_priority.load(std::memory_order_relaxed);
    }
    void set_memory_priority(u32 priority) noexcept {
        m_memory_priority.store(priority, std::memory_order_relaxed);
    }

    /// PROCESS_MODE_BACKGROUND_BEGIN/END latch for the current process.
    [[nodiscard]] bool background_mode() const noexcept {
        return m_background_mode;
    }
    void set_background_mode(bool on) noexcept { m_background_mode = on; }

    // -------------------------------------------------------------------------
    // Chain-Load (ShellExecuteW .exe launch)
    // -------------------------------------------------------------------------

    /// Store a pending launch request (instead of fork+exec).
    /// After the current guest exits, the runtime will chain-load this exe.
    void set_pending_launch(const std::string& host_path,
                            const std::vector<std::string>& args);

    /// Check whether a chain-load was requested
    [[nodiscard]] bool has_pending_launch() const noexcept;

    /// Take the pending launch info (clears it)
    std::pair<std::string, std::vector<std::string>> take_pending_launch();

    // -------------------------------------------------------------------------
    // Startup Info
    // -------------------------------------------------------------------------

    /// Write a zeroed STARTUPINFOW structure to guest memory
    /// @param m   Marshaler for guest memory access
    /// @param addr Guest address to write the structure
    void write_startup_info(Marshaler& m, GuestAddr addr);

private:
    std::atomic<bool> m_exited{false};
    u32  m_exit_code = 0;
    /// NORMAL_PRIORITY_CLASS — Learn GetPriorityClass default.
    u32  m_priority_class = 0x00000020u;
    /// MEMORY_PRIORITY_NORMAL (winnt.h).
    std::atomic<u32> m_memory_priority{5};
    bool m_background_mode = false;
    std::function<void()> m_exit_wake;

    std::string              m_pending_launch_path;
    std::vector<std::string> m_pending_launch_args;
    ProcessSnapshotProvider  m_process_snapshot_provider;
    ModuleManager*           m_module_manager = nullptr;
};

/// Report the guest call sites that led to a process ending itself.
///
/// A guest that calls ExitProcess or TerminateProcess on its own process
/// leaves no fault artifact, so without this the log records only that the
/// process stopped. Frame pointers are frequently absent on these paths (an
/// aborting CRT, a `[[noreturn]]` fatal handler), so this scans the live stack
/// and reports every slot that resolves inside a loaded PE module as
/// module+RVA. It is a best-effort report: stale slots can survive below the
/// stack pointer, so entries are candidates, ordered innermost first.
void log_self_termination_chain(Marshaler& m, const ModuleManager* modules,
                                const char* api);

// =============================================================================
// Win32 API Thunk Implementations
// =============================================================================

/// VOID WINAPI ExitProcess(UINT uExitCode);
void impl_ExitProcess(Marshaler& m, ProcessManager& pm);

/// HANDLE WINAPI GetCurrentProcess(void);
void impl_GetCurrentProcess(Marshaler& m, ProcessManager& pm);

/// DWORD WINAPI GetCurrentProcessId(void);
void impl_GetCurrentProcessId(Marshaler& m, ProcessManager& pm);

/// HANDLE WINAPI GetCurrentThread(void);
void impl_GetCurrentThread(Marshaler& m, ProcessManager& pm);

/// DWORD WINAPI GetCurrentThreadId(void);
void impl_GetCurrentThreadId(Marshaler& m, ProcessManager& pm);

/// BOOL WINAPI IsDebuggerPresent(void);
void impl_IsDebuggerPresent(Marshaler& m, ProcessManager& pm);

/// VOID WINAPI GetStartupInfoW(LPSTARTUPINFOW lpStartupInfo);
void impl_GetStartupInfoW(Marshaler& m, ProcessManager& pm);

// =============================================================================
// Export Registration
// =============================================================================

/// Build the DllExports table for process/thread APIs
/// @param pm ProcessManager instance to bind into thunk closures
/// @param gl Unused (FormatMessage lives in kernel32/text/format_message)
/// @return DllExports with all process/thread API entries
DllExports make_process_exports(ProcessManager& pm,
                                GlobalLocalManager* gl = nullptr);

/// Additional kernel32 exports that operate on process handles.
/// Includes OpenProcess and TerminateProcess.
DllExports make_process_handle_exports(ProcessManager& pm, HandleTable& ht);
/// ntdll's NtTerminateProcess, over the same process handles.
DllExports make_nt_process_handle_exports(ProcessManager& pm,
                                          HandleTable& ht);

/// Guest `SetUnhandledExceptionFilter` pointer (0 if none).
///
/// Windows invokes this filter after frame SEH returns ContinueSearch, on the
/// faulting thread. See errhandlingapi `SetUnhandledExceptionFilter`.
[[nodiscard]] u64 unhandled_exception_filter_guest() noexcept;
u64 set_unhandled_exception_filter_guest(u64 handler) noexcept;

/// Invokes a guest top-level exception filter with EXCEPTION_POINTERS*.
/// Wired from run.cpp via run_guest_callback so UnhandledExceptionFilter and
/// the SEH dispatchers share one calling path.
using UnhandledExceptionFilterInvoker =
    std::function<u64(GuestAddr filter, GuestAddr exception_pointers, bool is_64bit)>;
void set_unhandled_exception_filter_invoker(
    UnhandledExceptionFilterInvoker invoker) noexcept;

} // namespace vitreo::winapi
