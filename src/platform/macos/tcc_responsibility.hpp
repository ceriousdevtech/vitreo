// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file tcc_responsibility.hpp
/// @brief Which process macOS privacy consent (TCC) is asked for.
///
/// TCC attributes a privacy request (microphone, camera, Bluetooth, protected
/// folders) to the requesting process's *responsible* process: by default the
/// app that launched it, inherited across fork and exec. A guest started from
/// a shell inside another app (Terminal or another host app) is judged as
/// that app, and that app's recorded answer applies, including an earlier
/// "Don't Allow" that makes TCC refuse without asking (measured: tccd
/// "Handling access request to kTCCServiceMicrophone, from
/// Sub:<launching-app> ... Denied (User Consent)" for a Vitreo
/// guest). A process can instead be made responsible for itself when it is
/// spawned (the responsibility "disclaim" spawn attribute, which terminals and
/// IDEs use for the programs they run); TCC then judges the process's own
/// code, by its executable, and reads the usage strings from that code's
/// Info.plist.

#include <sys/types.h>

#include <string>

namespace vitreo::platform {

/// The responsible process of `pid`, or -1 when the host cannot say.
[[nodiscard]] pid_t responsible_process(pid_t pid);

/// Whether the host offers the disclaim spawn attribute.
[[nodiscard]] bool can_disclaim_responsibility();

/// posix_spawn `path` with the disclaim attribute, so the new image is its
/// own responsible process. `replace_current` uses POSIX_SPAWN_SETEXEC: the
/// current process becomes the new image (same pid, process group, parent
/// and descriptors, as execve) and the call returns only on failure.
/// Returns 0 or an errno value (ENOTSUP when the attribute is unavailable).
[[nodiscard]] int spawn_disclaiming_responsibility(pid_t* pid, const char* path,
                                                   char* const argv[], char* const envp[],
                                                   bool replace_current);

/// The executable of a running process ("" when unknown).
[[nodiscard]] std::string process_executable_path(pid_t pid);

/// Whether the Info.plist TCC reads for the code at `executable` declares
/// `key` (e.g. "NSMicrophoneUsageDescription"): the enclosing application
/// bundle's Info.plist for an executable at <App>.app/Contents/MacOS/<name>,
/// otherwise the executable's embedded __TEXT,__info_plist section. TCC
/// terminates a process whose responsible code lacks the usage string
/// instead of failing the request, so a caller checks this first.
[[nodiscard]] bool code_declares_usage(const std::string& executable, const char* key);

}  // namespace vitreo::platform
