#pragma once
/// @file launch_request.hpp
/// @brief What a guest process is launched with, and what it produces.
///
/// `RunConfig` and `RunResult` describe a guest launch and its outcome, so
/// they belong to the execution runtime rather than to the command-line front
/// end that happens to build one — the CLI is one caller, and a supervised
/// child worker is another.
///
/// Kept under their existing names deliberately: renaming them is a mechanical
/// change across every call site, and mixing that into a structural move would
/// touch every caller for a rename that changes nothing.

#include "ipc/process_session.hpp"
#include "core/pe/loader/teb_setup.hpp"

#include <vitreo/common.hpp>

#include <string>
#include <utility>
#include <vector>

namespace vitreo::ipc {
class ProcessSession;
}

namespace vitreo::platform {
class ElevationAuthorizer;
}

namespace vitreo::cli {

// =============================================================================
// Run Configuration
// =============================================================================

/// Configuration for a run session
struct RunConfig {
    std::string pe_path;                    ///< Path to the PE file on host
    std::string capsule_name;               ///< Capsule name (default: auto-generated)
    std::string app_name;                   ///< Explicit macOS presentation name
    std::vector<std::string> guest_args;    ///< Extra guest argv tokens (appended to command line)
    std::vector<std::pair<std::string, std::string>> guest_environment;
    bool verbose = false;                   ///< Verbose output
    u64 max_instructions = 0;               ///< Max instructions (0 = unlimited)
    std::string checkpoint_save_path;       ///< Save state when execution stops
    std::string checkpoint_restore_path;    ///< Restore state before guest replay
    u64 checkpoint_at = 0;                  ///< Save after this many instructions
    u64 replay_instructions = 0;            ///< Bounded instructions after restore
    u64 deterministic_rng_seed = 0;         ///< Recorded replay RNG seed metadata
    u64 stack_size = 0x100000;              ///< Default 1 MiB stack
    u64 stack_base = pe::PRIMARY_TEB_ADDRESS; ///< Stack top / primary TEB
    /// Set when this vitreo process was spawned by
    /// ProcessSession running on a /tmp snapshot of a
    /// guest executable. Holds the original Windows-side path that the guest
    /// asked to launch (e.g. "C:\\Program Files\\App\\app.exe"),
    /// so we can preserve GetCommandLineW/GetModuleFileNameW/CWD identity
    /// across the relaunch instead of leaking the snapshot path into the
    /// guest.
    std::string relaunched_as;
    /// Windows current directory inherited by a supervised child worker.
    std::string guest_cwd;
    /// Full Windows command line for a CreateProcess-spawned child (verbatim).
    /// When non-empty, GetCommandLineW returns this instead of a reconstructed
    /// exe-path-only line. A browser subprocess needs the verbatim line
    /// because its type, parent pid, and IPC channel all arrive via argv.
    std::string guest_cmdline;
    /// Internal serialized named-pipe handles inherited across CreateProcess.
    std::string inherited_pipes;
    /// "in,out" pseudoconsole pipe handles when this process was launched with
    /// PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE; its console IS that pseudoconsole.
    std::string conpty_handles;
    std::string inherited_sync;
    std::string inherited_mappings;
    /// Internal UTF-16LE hexadecimal StartService arguments. The service
    /// dispatcher marshals these into ServiceMain argv after argv[0].
    std::vector<std::string> guest_service_arguments_utf16;
    /// Windows creator process id captured by ProcessSession before fork.
    /// This remains valid even if the host creator exits before this worker
    /// finishes startup and macOS reparents the worker.
    u32 guest_parent_process_id{0};
    /// Raw Security.framework external authorization form.
    std::string elevation_credential;
    /// Parent-issued Windows token context for an SCM-managed worker.
    ipc::WorkerTokenContext token_context{ipc::WorkerTokenContext::User};
    /// Non-owning native authorization boundary owned by RunCommand.
    platform::ElevationAuthorizer* elevation_authorizer{nullptr};
    /// Non-owning supervisor for the user-facing `vitreo run` invocation.
    ipc::ProcessSession* process_session{nullptr};
};

// =============================================================================
// Run Result
// =============================================================================

/// Result of running a PE
struct RunResult {
    int exit_code = 0;                      ///< Process exit code
    u64 instructions_executed = 0;          ///< Total instructions executed
    std::string error_message;              ///< Error description (empty on success)
    bool success = false;                   ///< True if execution completed without error

    std::string pending_launch_path;        ///< Chain-load: next exe to run (host path)
    std::vector<std::string> pending_launch_args; ///< Chain-load: args for next exe
    bool process_elevated{false};            ///< State inherited by chain-loads
    std::string elevation_credential;        ///< Credential inherited by chain-loads
    ipc::WorkerTokenContext token_context{ipc::WorkerTokenContext::User};
};

} // namespace vitreo::cli
