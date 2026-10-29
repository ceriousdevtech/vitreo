// SPDX-License-Identifier: LGPL-2.1-only
/// @file scoped_environment.cpp
/// @brief VITREO_SCOPED_ENV delivery. See platform/scoped_environment.hpp.

#include "platform/scoped_environment.hpp"

#include <crt_externs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace vitreo::platform {

namespace {

constexpr std::string_view kArrow = "=>";

// Namespace-scope on purpose: this runs while the image's static initialisers
// run, the same moment and the same environment in which every latched knob
// reads its variable. `held` is therefore not a guess about what those knobs
// saw; it is a read taken where they read.
ScopedEnvStartup take_startup_snapshot() {
    ScopedEnvStartup startup;
    if (const char* text = std::getenv(kScopedEnvironmentVariable);
        text != nullptr && text[0] != '\0') {
        std::string error;
        if (const auto directive = parse_scoped_environment(text, error)) {
            for (const auto& assignment : directive->assignments) {
                const char* current = std::getenv(assignment.name.c_str());
                if (current != nullptr && assignment.value == current) {
                    startup.held.push_back(assignment);
                }
            }
        }
    }
    if (const char* marker = std::getenv(kScopedEnvironmentReexecMarker);
        marker != nullptr) {
        char* end = nullptr;
        const long pid = std::strtol(marker, &end, 10);
        startup.reexeced = end != marker && *end == '\0' &&
                           pid == static_cast<long>(::getpid());
    }
    return startup;
}

ScopedEnvStartup g_startup = take_startup_snapshot();

std::string join_names(const std::vector<ScopedEnvAssignment>& assignments) {
    std::string names;
    for (const auto& assignment : assignments) {
        if (!names.empty()) names += ',';
        names += assignment.name + '=' + assignment.value;
    }
    return names;
}

}  // namespace

bool ScopedEnvStartup::holds(const ScopedEnvAssignment& assignment) const {
    return std::find(held.begin(), held.end(), assignment) != held.end();
}

std::optional<ScopedEnvironment> parse_scoped_environment(
    std::string_view text, std::string& error) {
    const std::size_t arrow = text.find(kArrow);
    if (arrow == std::string_view::npos) {
        error = "no '=>' between the guest command-line substring and the "
                "assignments";
        return std::nullopt;
    }
    if (arrow == 0) {
        error = "the guest command-line substring before '=>' is empty";
        return std::nullopt;
    }

    ScopedEnvironment directive;
    directive.needle = std::string(text.substr(0, arrow));
    std::string_view rest = text.substr(arrow + kArrow.size());
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        const std::string_view item = rest.substr(0, comma);
        if (!item.empty()) {
            const std::size_t equals = item.find('=');
            if (equals == std::string_view::npos || equals == 0) {
                directive.rejected_items.emplace_back(item);
            } else {
                ScopedEnvAssignment assignment{
                    std::string(item.substr(0, equals)),
                    std::string(item.substr(equals + 1))};
                // A repeated name takes its last value, as consecutive
                // setenv calls would.
                const auto existing = std::find_if(
                    directive.assignments.begin(), directive.assignments.end(),
                    [&](const ScopedEnvAssignment& earlier) {
                        return earlier.name == assignment.name;
                    });
                if (existing != directive.assignments.end()) {
                    existing->value = std::move(assignment.value);
                } else {
                    directive.assignments.push_back(std::move(assignment));
                }
            }
        }
        if (comma == std::string_view::npos) break;
        rest = rest.substr(comma + 1);
    }

    if (directive.assignments.empty()) {
        error = "no NAME=VALUE assignment after '=>'";
        if (!directive.rejected_items.empty()) {
            error += " (not NAME=VALUE: '" + directive.rejected_items.front() +
                     "')";
        }
        return std::nullopt;
    }
    return directive;
}

bool scoped_environment_matches(const ScopedEnvironment& directive,
                                std::string_view guest_command_line) noexcept {
    return guest_command_line.find(directive.needle) !=
           std::string_view::npos;
}

std::vector<ScopedEnvAssignment> scoped_environment_missing(
    const ScopedEnvironment& directive) {
    std::vector<ScopedEnvAssignment> missing;
    for (const auto& assignment : directive.assignments) {
        const char* current = std::getenv(assignment.name.c_str());
        if (current == nullptr || assignment.value != current) {
            missing.push_back(assignment);
        }
    }
    return missing;
}

std::vector<std::string> environment_with_scoped_assignments(
    const std::vector<std::string>& environment,
    const std::vector<ScopedEnvAssignment>& assignments, long pid) {
    const auto replaced = [&](std::string_view name) {
        if (name == kScopedEnvironmentReexecMarker) return true;
        return std::any_of(assignments.begin(), assignments.end(),
                           [&](const ScopedEnvAssignment& assignment) {
                               return assignment.name == name;
                           });
    };

    std::vector<std::string> result;
    result.reserve(environment.size() + assignments.size() + 1);
    for (const auto& entry : environment) {
        const std::string_view name =
            std::string_view(entry).substr(0, entry.find('='));
        if (!replaced(name)) result.push_back(entry);
    }
    for (const auto& assignment : assignments) {
        result.push_back(assignment.name + '=' + assignment.value);
    }
    result.push_back(std::string(kScopedEnvironmentReexecMarker) + '=' +
                     std::to_string(pid));
    return result;
}

const ScopedEnvStartup& scoped_environment_startup() noexcept {
    return g_startup;
}

void deliver_scoped_environment(const ScopedEnvironment& directive,
                                const ScopedEnvStartup& startup,
                                std::FILE* log) {
    for (const auto& item : directive.rejected_items) {
        std::fprintf(log,
                     "[process] scoped env: item '%s' IGNORED: expected "
                     "NAME=VALUE\n",
                     item.c_str());
    }
    for (const auto& assignment : directive.assignments) {
        const char* name = assignment.name.c_str();
        const char* value = assignment.value.c_str();
        if (startup.holds(assignment)) {
            std::fprintf(log,
                         startup.reexeced
                             ? "[process] scoped env: %s=%s (in the "
                               "environment before static initialisation: "
                               "re-executed with it)\n"
                             : "[process] scoped env: %s=%s (already in the "
                               "environment at process start)\n",
                         name, value);
            continue;
        }

        // Not in the environment when the knobs were read. Setting it now
        // still reaches every variable read lazily, so do that, and say what
        // it cannot reach.
        const char* reason =
            startup.reexeced
                ? "it was missing again after the start-up re-exec"
                : "the guest command line was not known at process start: the "
                  "image runs from outside the capsule, or was chain-loaded";
        char errno_reason[96];
        if (startup.reexec_errno != 0) {
            std::snprintf(errno_reason, sizeof(errno_reason),
                          "the start-up re-exec failed: %s",
                          std::strerror(startup.reexec_errno));
            reason = errno_reason;
        }
        if (::setenv(name, value, 1) != 0) {
            std::fprintf(log,
                         "[process] scoped env: %s=%s NOT APPLIED: setenv "
                         "failed: %s\n",
                         name, value, std::strerror(errno));
            continue;
        }
        std::fprintf(log,
                     "[process] scoped env: %s=%s LATE: applied after "
                     "process start (%s), so knobs read at static "
                     "initialisation did NOT see it; set it on the whole "
                     "process tree to reach those\n",
                     name, value, reason);
    }
}

void deliver_scoped_environment_for_guest(std::string_view guest_command_line,
                                          std::FILE* log) {
    const char* text = std::getenv(kScopedEnvironmentVariable);
    if (text == nullptr || text[0] == '\0') return;

    std::string error;
    const auto directive = parse_scoped_environment(text, error);
    if (!directive) {
        // A directive that selects nothing must not read as a clean result.
        std::fprintf(log,
                     "[process] scoped env: %s IGNORED: %s (expected "
                     "<substring>=><NAME>=<VALUE>[,<NAME>=<VALUE>...])\n",
                     kScopedEnvironmentVariable, error.c_str());
        return;
    }
    if (!scoped_environment_matches(*directive, guest_command_line)) return;
    deliver_scoped_environment(*directive, g_startup, log);
}

void reexec_for_scoped_environment(std::string_view guest_command_line,
                                   const char* executable_path,
                                   char* const argv[], std::FILE* log) {
    const char* text = std::getenv(kScopedEnvironmentVariable);
    if (text == nullptr || text[0] == '\0') return;

    std::string error;
    const auto directive = parse_scoped_environment(text, error);
    // A malformed directive is reported when the guest launches.
    if (!directive || !scoped_environment_matches(*directive, guest_command_line)) {
        return;
    }
    const auto missing = scoped_environment_missing(*directive);
    if (missing.empty()) return;

    if (g_startup.reexeced) {
        // The previous image was given these values and one of them is gone:
        // this process (or code it ran) changed them. Another exec would
        // repeat that without end.
        std::fprintf(log,
                     "[process] scoped env: %s still missing from the "
                     "environment after the start-up re-exec; not "
                     "re-executing again\n",
                     join_names(missing).c_str());
        return;
    }
    if (executable_path == nullptr || executable_path[0] == '\0') {
        g_startup.reexec_errno = ENOENT;
        std::fprintf(log,
                     "[process] scoped env: cannot re-execute to deliver %s: "
                     "the executable path is unknown\n",
                     join_names(missing).c_str());
        return;
    }

    std::vector<std::string> current;
    for (char* const* entry = *_NSGetEnviron();
         entry != nullptr && *entry != nullptr; ++entry) {
        current.emplace_back(*entry);
    }
    std::vector<std::string> next = environment_with_scoped_assignments(
        current, missing, static_cast<long>(::getpid()));
    std::vector<char*> envp;
    envp.reserve(next.size() + 1);
    for (auto& entry : next) envp.push_back(entry.data());
    envp.push_back(nullptr);

    std::fprintf(log,
                 "[process] scoped env: re-executing to deliver %s before "
                 "static initialisation\n",
                 join_names(missing).c_str());
    std::fflush(log);
    ::execve(executable_path, argv, envp.data());

    g_startup.reexec_errno = errno;
    std::fprintf(log,
                 "[process] scoped env: re-exec of '%s' FAILED: %s; the "
                 "values will be applied after process start instead\n",
                 executable_path, std::strerror(g_startup.reexec_errno));
}

}  // namespace vitreo::platform
