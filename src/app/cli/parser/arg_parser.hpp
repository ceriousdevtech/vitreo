#pragma once
#include <vitreo/common.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <optional>

namespace vitreo::cli {

/// Parsed command-line arguments
struct ParsedArgs {
    std::string command;                        // "run", "capsule", "config", "version", "help"
    std::string subcommand;                     // "create", "list", "destroy", "get", "set" (if applicable)
    std::vector<std::string> positional_args;   // Remaining positional arguments
    
    // Named options
    bool verbose = false;
    bool help = false;
    bool all = false;
    bool json = false;                         // --json (machine-readable output)
    bool interpreter = false;                   // --interpreter / --no-jit (force interpreter)
    bool trace = false;                         // --trace (verbose/trace logging)
    std::optional<std::string> checkpoint_save; // --checkpoint-save <file>
    std::optional<std::string> checkpoint_restore; // --checkpoint-restore <file>
    std::optional<std::string> checkpoint_at;   // --checkpoint-at <instructions>
    std::optional<std::string> replay_instructions; // --replay-instructions <count>
    std::optional<std::string> replay_seed;      // --replay-seed <u64>
    std::optional<std::string> capsule_name;    // --capsule <name>
    /// User/shortcut-provided macOS presentation name for this launch.
    std::optional<std::string> app_name;        // --app-name <name>
    std::optional<std::string> drive_letter;    // --drive <letter>
    std::vector<std::string> guest_environment; // --env NAME=VALUE (repeatable)
    std::optional<std::string> config_key;      // For config commands
    std::optional<std::string> config_value;    // For config set
    /// Internal flag set by ProcessSession when relaunching
    /// vitreo on a /tmp snapshot of a guest executable. Carries the original
    /// Windows-side path so the relaunched process can present
    /// GetCommandLineW/GetModuleFileNameW/CWD as if it were still running from
    /// the install dir.
    std::optional<std::string> relaunched_as;   // --relaunched-as <winpath>
    /// Internal Windows current directory inherited by a supervised worker.
    std::optional<std::string> guest_cwd;       // --guest-cwd <winpath>
    /// Full Windows command line for a CreateProcess-spawned child (one argv
    /// element). When set, GetCommandLineW returns this verbatim instead of a
    /// reconstructed "exe-path-only" line. Required for browser subprocesses,
    /// which receive their type, role, IPC channel, and parent pid entirely
    /// via argv.
    std::optional<std::string> guest_cmdline;    // --guest-cmdline <full cmdline>
    std::optional<std::string> inherited_pipes;  // --inherited-pipes <descriptors>
    // --conpty-handles <in>,<out>: this child's console IS the named
    // pseudoconsole; its console output goes to <out> as VT.
    std::optional<std::string> conpty_handles;
    std::optional<std::string> inherited_sync;   // --inherited-sync <descriptors>
    std::optional<std::string> inherited_mappings; // --inherited-mappings <descriptors>
    /// Internal UTF-16LE hexadecimal StartService argument, repeated once per
    /// argument. These become ServiceMain argv[1..], never process argv.
    std::vector<std::string> guest_service_arguments_utf16;
    /// Internal Security.framework external authorization form (hex encoded).
    std::optional<std::string> guest_elevation_credential;
    /// Internal inherited descriptor carrying an SCM-selected token context.
    /// The descriptor payload, not this option alone, is the authority.
    std::optional<std::string> guest_token_context_fd;
    /// Internal Windows creator process id captured before fork. Host
    /// getppid() is not a stable substitute because a short-lived creator may
    /// exit before the worker initializes and macOS reparents it to launchd.
    std::optional<std::string> guest_parent_pid;
    /// Internal inherited read descriptor of the parent-death pipe. The worker
    /// watches it and self-terminates when the owning process closes the sole
    /// write end (i.e. exits for any reason), preventing orphaned process trees.
    std::optional<std::string> parent_death_fd;
    /// Internal inherited write descriptor for the full Windows ExitProcess
    /// DWORD. Host wait status only keeps 8 bits; the worker writes the real
    /// code here before exiting so the parent GetExitCodeProcess is accurate.
    std::optional<std::string> guest_exit_code_fd;
};

/// Parse command-line arguments
/// @param argc Argument count
/// @param argv Argument values
/// @return Parsed arguments or error string
struct ParseResult {
    bool success;
    ParsedArgs args;
    std::string error;
    
    static ParseResult ok(ParsedArgs a) { return {true, std::move(a), {}}; }
    static ParseResult err(std::string e) { return {false, {}, std::move(e)}; }
};

[[nodiscard]] inline usize guest_environment_separator(
    std::string_view entry) noexcept {
    return entry.find('=', entry.starts_with('=') ? 1 : 0);
}

[[nodiscard]] ParseResult parse_args(int argc, const char* const* argv);

} // namespace vitreo::cli
