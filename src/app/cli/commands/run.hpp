// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CLI-002: vitreoctl run command

#pragma once

#include "../parser/arg_parser.hpp"
#include "command.hpp"
#include "runtime/execution/launch_request.hpp"
#include "ipc/process_session.hpp"
#include "core/pe/loader/teb_setup.hpp"
#include <vitreo/common.hpp>
#include <optional>
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
// Core Execution Pipeline
// =============================================================================

/// Derive the macOS application name from a Windows executable path.
/// Directory components and a case-insensitive .exe suffix are removed.
[[nodiscard]] std::string application_name_from_windows_path(
    std::string_view windows_path);

/// Windows application presentation metadata available before guest startup.
struct ApplicationLaunchMetadata {
    std::vector<u8> icon_data;
    std::string product_name;
    std::string file_description;
};

/// Choose a stable macOS presentation name in this order:
/// explicit launch name, PE FileDescription, PE ProductName, executable name.
[[nodiscard]] std::string select_application_display_name(
    std::string_view explicit_name,
    const ApplicationLaunchMetadata& metadata,
    std::string_view windows_path);

/// Best-effort extraction of icon and version-resource naming metadata.
[[nodiscard]] ApplicationLaunchMetadata extract_application_metadata_for_launch(
    const std::string& pe_path,
    const std::string& capsule_name);

/// Compatibility helper returning only the prelaunch icon.
[[nodiscard]] std::vector<u8> extract_application_icon_for_launch(
    const std::string& pe_path,
    const std::string& capsule_name);

/// Set the RunConfig fields that decide which Windows command line the guest
/// runs with, from the parsed command line: the executable (`pe_path`), the
/// guest arguments, the capsule, and the launcher-supplied `--relaunched-as` /
/// `--guest-cmdline`. Requires a non-empty `args.positional_args`.
void assign_launch_identity(RunConfig& config, const ParsedArgs& args);

/// The Windows command line `config` gives the guest, when the arguments and
/// the capsule's layout decide it alone (so before any state is created):
/// `--guest-cmdline` verbatim, or `"<exe path>" <args>`. nullopt when the guest
/// executable lies outside the capsule and its guest name is still to be
/// chosen. Touches nothing.
[[nodiscard]] std::optional<std::string> known_guest_command_line(
    const RunConfig& config);

/// VITREO_SCOPED_ENV delivery at the earliest point a process can match it
/// (platform/scoped_environment.hpp): when the variable selects this process's
/// guest and its environment lacks the values, replace the process with a new
/// image of the executable it was started as that has them, before any static
/// initialiser can latch a knob without them. Call first thing in `main`,
/// before any thread exists. Returns when there is nothing to do (no
/// VITREO_SCOPED_ENV, not a `run`, a guest command line that is only known
/// once its executable is published into the capsule) or the exec failed; the
/// failure is reported, and execute_pe then reports the values it applies late.
void reexec_for_scoped_environment(int argc, char* argv[]);

/// Execute a PE file in a sandboxed environment
///
/// Pipeline:
///   1. Read PE file from disk
///   2. Parse PE headers
///   3. Set up guest memory
///   4. Map sections into guest memory
///   5. Apply relocations (if needed)
///   6. Set up thunk registry for API emulation
///   7. Resolve and patch imports
///   8. Set up CPU state (RIP = entry point, RSP = stack)
///   9. Run interpreter loop
///
/// @param config Execution configuration
/// @return RunResult with exit code, instruction count, and error info
RunResult execute_pe(const RunConfig& config);

// =============================================================================
// Run Command
// =============================================================================

/// CLI command for running PE files
///
/// Usage: vitreoctl run <pe-path> [--capsule <name>]
///                      [--app-name <name>] [--verbose]
class RunCommand : public Command {
public:
    CommandResult execute(const ParsedArgs& args) override;
    std::string_view name() const override { return "run"; }
    std::string_view help() const override { return "Execute a Windows PE executable"; }
};

} // namespace vitreo::cli
