// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file scoped_environment.hpp
/// @brief VITREO_SCOPED_ENV: host environment variables for one process of a
///        guest process tree.
///
/// A guest process tree (the browser, GPU and renderer processes of one
/// multi-process program; a launcher and the game it starts) is one tree of
/// host processes that inherit ONE environment. A measurement or diagnostic switch meant for a
/// single process of that tree (a JIT cost probe the rest of the tree cannot
/// run under) has no other way to reach only that process.
///
///     VITREO_SCOPED_ENV='<substring>=><NAME>=<VALUE>[,<NAME>=<VALUE>...]'
///
/// selects every process whose guest command line contains `<substring>` and
/// gives it `NAME=VALUE`. Values cannot contain a comma. Nothing in the source
/// names a guest.
///
/// **Why delivery is by re-exec.** Most Vitreo switches are read ONCE, into a
/// namespace-scope constant, while the image's static initialisers run (a bare
/// `getenv` on a hot path costs an environ lock; see jit_finalize.cpp). Those
/// run before `main`, long before any guest command line exists, so a
/// `setenv` made when the guest launches reaches only knobs read lazily. That
/// made the delivery a silent no-op for the switches that matter most
/// (`VITREO_JIT_MAP`, `VITREO_AB_CYCLE_S`): the process printed
/// "scoped env: X=1", the knob read nothing, and the run read as a clean
/// negative result. A process that matches and does not yet hold the values
/// therefore re-executes ITSELF (same pid, same argv, same descriptors) with
/// them in its environment, as the first thing `main` does, so the new image's
/// static initialisers see them. Whatever cannot be delivered that way is
/// applied late and SAYS so, per variable.
///
/// Process-wide state this module owns:
///   * a snapshot taken by a static initialiser of this file, i.e. at the same
///     moment, in the same environment, as the knobs it describes;
///   * `VITREO_SCOPED_ENV_REEXEC=<pid>`, written into the environment of the
///     re-executed image. exec keeps the pid, so a value equal to the
///     process's own pid proves "this image is the product of that re-exec"
///     (and bounds the re-exec to one); a descendant, whose pid differs, ignores
///     an inherited copy.

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::platform {

/// Name of the environment variable that carries the directive.
inline constexpr const char* kScopedEnvironmentVariable = "VITREO_SCOPED_ENV";

/// Name of the pid-valued marker the re-exec leaves in the new image.
inline constexpr const char* kScopedEnvironmentReexecMarker =
    "VITREO_SCOPED_ENV_REEXEC";

struct ScopedEnvAssignment {
    std::string name;
    std::string value;

    friend bool operator==(const ScopedEnvAssignment&,
                           const ScopedEnvAssignment&) = default;
};

/// A parsed VITREO_SCOPED_ENV directive.
struct ScopedEnvironment {
    /// Text a guest command line must contain for the directive to apply.
    std::string needle;
    std::vector<ScopedEnvAssignment> assignments;
    /// Comma-separated items that were not `NAME=VALUE` (no `=`, or an empty
    /// name). They are never applied; they are reported.
    std::vector<std::string> rejected_items;
};

/// Parse the text of VITREO_SCOPED_ENV.
///
/// Accepts `<substring>=><NAME>=<VALUE>[,<NAME>=<VALUE>...]`. The substring is
/// everything before the first `=>` and must not be empty; items split on
/// commas, and each at its first `=`, so a value may contain `=` but not a
/// comma. Empty items (`A=1,,B=2`, a trailing comma) are skipped. Returns
/// nullopt with `error` describing the first defect when the text carries no
/// substring, or no `NAME=VALUE` assignment at all.
[[nodiscard]] std::optional<ScopedEnvironment> parse_scoped_environment(
    std::string_view text, std::string& error);

/// True when `guest_command_line` contains the directive's substring.
[[nodiscard]] bool scoped_environment_matches(
    const ScopedEnvironment& directive,
    std::string_view guest_command_line) noexcept;

/// The directive's assignments the CURRENT process environment does not hold
/// (name unset, or set to a different value).
[[nodiscard]] std::vector<ScopedEnvAssignment> scoped_environment_missing(
    const ScopedEnvironment& directive);

/// `environment` (a `NAME=VALUE` list, as the process environ) with every
/// entry of a name in `assignments` replaced by the assignment, plus the
/// re-exec marker for `pid`. Entries of other names keep their order.
[[nodiscard]] std::vector<std::string> environment_with_scoped_assignments(
    const std::vector<std::string>& environment,
    const std::vector<ScopedEnvAssignment>& assignments, long pid);

/// What this process image knew when it started.
struct ScopedEnvStartup {
    /// Assignments of the directive the environment ALREADY held when this
    /// image's static initialisers ran: the ones every latched knob saw.
    std::vector<ScopedEnvAssignment> held;
    /// This image is the product of this module's re-exec.
    bool reexeced = false;
    /// errno of a re-exec that was attempted and failed (0: none).
    int reexec_errno = 0;

    [[nodiscard]] bool holds(const ScopedEnvAssignment& assignment) const;
};

/// The snapshot of the running image.
[[nodiscard]] const ScopedEnvStartup& scoped_environment_startup() noexcept;

/// Deliver a MATCHED directive for a process that is about to run a guest, and
/// say, per assignment, what happened to it on `log`:
///   * held at image start (by re-exec, or because the tree already had it):
///     every knob saw it;
///   * not held: applied now with setenv, and reported as LATE, with the
///     reason. Knobs latched at static initialisation did not see it.
/// An item that was not `NAME=VALUE` is reported and skipped.
void deliver_scoped_environment(const ScopedEnvironment& directive,
                                const ScopedEnvStartup& startup,
                                std::FILE* log);

/// deliver_scoped_environment for the running process: parse
/// VITREO_SCOPED_ENV, report a malformed one (never silently), match it
/// against `guest_command_line`, deliver. No output when the variable is unset
/// or the directive does not select this guest.
void deliver_scoped_environment_for_guest(std::string_view guest_command_line,
                                          std::FILE* log);

/// As early as a process can know its guest command line: when
/// VITREO_SCOPED_ENV selects it and the environment lacks a value, replace the
/// process with a new image of `executable_path` (same pid and argv, inherited
/// descriptors) whose environment holds them, so the new image's static
/// initialisers see them. Call it before any thread exists.
///
/// Returns only when there was nothing to do, the process had already been
/// re-executed (a second exec cannot help, and must not loop), or the exec
/// failed; the failure is reported on `log` and remembered for
/// deliver_scoped_environment.
void reexec_for_scoped_environment(std::string_view guest_command_line,
                                   const char* executable_path,
                                   char* const argv[], std::FILE* log);

}  // namespace vitreo::platform
